#!/usr/bin/env python3
"""Gera um GGUF minúsculo da arquitetura Granite para testes de integração.

Este utilitário existe apenas para o CI e para o desenvolvedor: ele produz um
arquivo GGUF com a arquitetura real `granite` (GQA, RoPE, SwiGLU, RMSNorm e
embeddings de entrada/saída separadas) e um vocabulário pequeno, para que o
servidor real do llama.cpp possa ser iniciado em segundos sem baixar os
2,2 GB do Granite 4.2 3B Q4_K_M.

Duas honestidades importantes:

* os pesos são pseudoaleatórios com semente fixa, então o texto gerado não
  tem significado — o modelo não "sabe" nada;
* a cabeça de saída é restringida a bytes imprimíveis porque o servidor do
  llama.cpp valida a saída contra o template de chat em PEG, e bytes
  aleatórios formariam UTF-8 inválido e derrubariam a geração com HTTP 500.

O que este modelo valida de verdade: carregamento do GGUF, arquitetura
`granite`, template de chat com `enable_thinking`/`low_effort`, separação de
`reasoning_content`, sampler, streaming e o supervisor de processo do Cortex.
A validação com os pesos reais do Granite fica no workflow
`granite-real-model.yml`, que baixa o modelo oficial.

Requisitos: numpy e o pacote `gguf` do llama.cpp (`--llama-dir` ou
`LLAMA_CPP_DIR` aponta para o checkout).
"""

from __future__ import annotations

import argparse
import math
import pathlib
import sys

import numpy as np

# Tokens especiais do Granite 4.x. Os nomes precisam bater com o template de
# chat para que o parser do llama.cpp reconheça as marcações de papel.
SPECIAL_TOKENS = [
    "<|start_of_text|>",
    "<|end_of_text|>",
    "<|start_of_role|>",
    "<|end_of_role|>",
    "<think>",
    "</think>",
]

# Formato Granite 4.x: <|start_of_role|>papel<|end_of_role|>conteúdo<|end_of_text|>.
# `enable_thinking` abre o bloco  de raciocínio e `low_effort` encurta o
# esforço, exatamente como documentado pela IBM para o Granite 4.2.
GRANITE_CHAT_TEMPLATE = (
    "{%- for message in messages -%}"
    "{{- '<|start_of_role|>' + message['role'] + '<|end_of_role|>' -}}"
    "{%- if message.get('content') is string -%}"
    "{%- if message['role'] == 'assistant' and enable_thinking is defined"
    " and not enable_thinking and '' + message['content'] -%}"
    "{{- message['content'].split('')[-1] -}}"
    "{%- else -%}"
    "{{- message['content'] -}}"
    "{%- endif -%}"
    "{%- endif -%}"
    "{{- '<|end_of_text|>' -}}"
    "{%- endfor -%}"
    "{%- if add_generation_prompt -%}"
    "{{- '<|start_of_role|>assistant<|end_of_role|>' -}}"
    "{%- if enable_thinking is defined and enable_thinking is true -%}"
    "{%- if low_effort is defined and low_effort is true -%}"
    "{{- '{reasoning effort: low}' -}}"
    "{%- endif -%}"
    "{{- '' -}}"
    "{%- endif -%}"
    "{%- endif -%}"
)

# Bytes que o fixture pode emitir: texto imprimível, espaço e quebras de linha.
PRINTABLE_BYTES = set(range(0x20, 0x7F)) | {0x09, 0x0A}


def gpt2_byte_table() -> tuple[list[str], dict[int, int]]:
    """Réplica da tabela de bytes do BPE estilo GPT-2.

    Devolve a lista de tokens (um por valor de byte) e o índice de cada byte.
    """
    visible = list(range(ord("!"), ord("~") + 1))
    visible += list(range(ord("\u00a1"), ord("\u00ac") + 1))
    visible += list(range(ord("\u00ae"), ord("\u00ff") + 1))
    shifted = [byte for byte in range(256) if byte not in set(visible)]

    tokens: list[str] = []
    byte_to_index: dict[int, int] = {}
    for byte in visible:
        byte_to_index[byte] = len(tokens)
        tokens.append(chr(byte))
    for position, byte in enumerate(shifted):
        byte_to_index[byte] = len(tokens)
        tokens.append(chr(256 + position))
    return tokens, byte_to_index


def build_tokenizer(vocab_extra: int) -> tuple[list[str], list[int], list[str], set[int]]:
    """Devolve (tokens, tipos, merges, índices imprimíveis).

    Tipos seguem o enum do llama.cpp: 1 = NORMAL, 3 = CONTROL. Tokens de byte
    são NORMAL, como no vocabulário BPE real do Granite; o tipo BYTE (6) é
    reservado ao padrão SentencePiece `<0xXX>` e quebraria o carregamento.
    O llama.cpp exige uma tabela de merges não vazia para BPE e cada resultado
    de merge precisa existir no vocabulário, então pares de letras vizinhas do
    alfabeto são registrados como token e como regra.
    """
    byte_tokens, byte_to_index = gpt2_byte_table()
    tokens = list(byte_tokens)
    types = [1] * len(tokens)

    merges: list[str] = []
    alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    for index in range(0, len(alphabet) - 1, 2):
        first, second = alphabet[index], alphabet[index + 1]
        tokens.append(first + second)
        types.append(1)
        merges.append(f"{first} {second}")

    for index in range(vocab_extra):
        tokens.append(f"<extra_{index}>")
        types.append(3)
    for special in SPECIAL_TOKENS:
        tokens.append(special)
        types.append(3)

    printable_indices = {byte_to_index[byte] for byte in PRINTABLE_BYTES
                         if byte in byte_to_index}
    return tokens, types, merges, printable_indices


def build_tensors(dim: int, layers: int, heads: int, kv_heads: int, ffn: int,
                  vocab: int, printable_indices: set[int],
                  seed: int) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    head_dim = dim // heads

    def normal(shape: tuple[int, ...], scale: float) -> np.ndarray:
        return (rng.standard_normal(shape) * scale).astype(np.float32)

    embed_scale = 1.0 / float(np.sqrt(dim))
    tensors: dict[str, np.ndarray] = {}

    # Cabeça de saída: mantém pesos reais nas posições de texto imprimível e
    # suprime o resto com um valor muito negativo. Assim a amostragem nunca
    # escolhe um token que produziria UTF-8 inválido para o parser do servidor.
    # Usa -inf (e não um valor apenas muito negativo): com pesos aleatórios os
    # logits podem crescer a ponto de superar qualquer penalidade finita, e aí
    # a amostragem voltaria a escolher tokens que formam UTF-8 inválido.
    # Só a cabeça de saída é restringida: o embedding precisa continuar
    # normal, porque os tokens especiais do prompt também entram por ele.
    output = normal((vocab, dim), embed_scale)
    for token in range(vocab):
        if token not in printable_indices:
            output[token, :] = float("-inf")
    tensors["output.weight"] = output

    # Granite 4.x não compartilha pesos entre entrada e saída.
    tensors["token_embd.weight"] = normal((vocab, dim), embed_scale)
    tensors["output_norm.weight"] = np.ones(dim, dtype=np.float32)

    for layer in range(layers):
        prefix = f"blk.{layer}."
        tensors[prefix + "attn_norm.weight"] = np.ones(dim, dtype=np.float32)
        tensors[prefix + "attn_q.weight"] = normal((heads * head_dim, dim), embed_scale)
        tensors[prefix + "attn_k.weight"] = normal((kv_heads * head_dim, dim), embed_scale)
        tensors[prefix + "attn_v.weight"] = normal((kv_heads * head_dim, dim), embed_scale)
        tensors[prefix + "attn_output.weight"] = normal((dim, heads * head_dim), embed_scale)
        tensors[prefix + "ffn_norm.weight"] = np.ones(dim, dtype=np.float32)
        tensors[prefix + "ffn_gate.weight"] = normal((ffn, dim), embed_scale)
        tensors[prefix + "ffn_up.weight"] = normal((ffn, dim), embed_scale)
        tensors[prefix + "ffn_down.weight"] = normal((dim, ffn), embed_scale)
    return tensors


def main() -> int:
    parser = argparse.ArgumentParser(description="GGUF Granite mínimo para testes")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--llama-dir", type=pathlib.Path,
                        default=pathlib.Path("."),
                        help="checkout do llama.cpp com gguf-py/gguf")
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--layers", type=int, default=4)
    parser.add_argument("--heads", type=int, default=4)
    parser.add_argument("--kv-heads", type=int, default=2)
    parser.add_argument("--ffn", type=int, default=256)
    parser.add_argument("--context", type=int, default=2048)
    parser.add_argument("--vocab-extra", type=int, default=16)
    parser.add_argument("--seed", type=int, default=20260912)
    arguments = parser.parse_args()

    gguf_path = arguments.llama_dir / "gguf-py"
    if not gguf_path.is_dir():
        print(f"erro: gguf-py não encontrado em {gguf_path}", file=sys.stderr)
        return 2
    sys.path.insert(0, str(gguf_path))
    from gguf import GGUFWriter  # importado tarde: depende do caminho acima.

    tokens, types, merges, printable_indices = build_tokenizer(arguments.vocab_extra)
    vocab_size = len(tokens)
    tensors = build_tensors(arguments.dim, arguments.layers, arguments.heads,
                            arguments.kv_heads, arguments.ffn, vocab_size,
                            printable_indices, arguments.seed)

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    writer = GGUFWriter(str(arguments.output), "granite")
    writer.add_name("granite-tiny-ci")
    writer.add_context_length(arguments.context)
    writer.add_embedding_length(arguments.dim)
    writer.add_block_count(arguments.layers)
    writer.add_feed_forward_length(arguments.ffn)
    writer.add_head_count(arguments.heads)
    writer.add_head_count_kv(arguments.kv_heads)
    writer.add_layer_norm_rms_eps(1e-5)
    writer.add_rope_freq_base(10000000.0)
    # Multiplicadores do Granite 4.x, com os mesmos nomes usados na conversão
    # oficial (attention/embedding/residual/logit scale).
    # As chaves abaixo existem no Granite real e precisam estar presentes para
    # exercitar o caminho de carregamento da arquitetura. Os valores são
    # adequados a esta rede minúscula em vez de copiar 1/128, 12, 0,22 e 8 do
    # 3B: com dim=128 aquelas escalas explodiriam a ativação e tornariam a
    # saída imprevisível.
    writer.add_attention_scale(1.0 / math.sqrt(arguments.dim // arguments.heads))
    writer.add_embedding_scale(1.0)
    writer.add_residual_scale(1.0)
    writer.add_logit_scale(1.0)
    writer.add_file_type(0)
    writer.add_vocab_size(vocab_size)
    writer.add_tokenizer_model("gpt2")
    writer.add_tokenizer_pre("default")
    writer.add_token_list(tokens)
    writer.add_token_types(types)
    writer.add_token_merges(merges)
    writer.add_chat_template(GRANITE_CHAT_TEMPLATE)
    writer.add_eos_token_id(tokens.index("<|end_of_text|>"))
    for name, tensor in tensors.items():
        writer.add_tensor(name, tensor.astype(np.float32))
    writer.write_header_to_file()
    writer.write_kv_data_to_file()
    writer.write_tensors_to_file()
    writer.close()

    total = sum(int(tensor.size) for tensor in tensors.values())
    print(f"GGUF Granite mínimo: {arguments.output} "
          f"({arguments.output.stat().st_size} bytes, {total} pesos, "
          f"vocab={vocab_size}, dim={arguments.dim}, camadas={arguments.layers}, "
          f"head_dim={arguments.dim // arguments.heads}, "
          f"tokens imprimíveis={len(printable_indices)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
