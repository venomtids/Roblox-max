#!/usr/bin/env python3
"""Teste de integração do caminho Granite/GGUF do Cortex, ponta a ponta.

O harness sobe `cortex serve`, deixa o Cortex iniciar o llama-server por conta
própria (é assim que o usuário roda no Windows), conversa com a API HTTP e
verifica contrato, telemetria, template de chat e encerramento do processo
filho. Só usa a biblioteca padrão, então roda no Linux e no Windows.

Dois modos:

* `--expect fixture`: usa o GGUF minúsculo gerado por
  `ci/tools/make_tiny_granite_gguf.py`. Os pesos são aleatórios, então o texto
  não tem sentido e o esperado é o filtro de qualidade rejeitar. O que se
  valida aqui é o encanamento: supervisão do processo, template Granite,
  `chat_template_kwargs`, parsing da resposta e telemetria.
* `--expect real`: usa os pesos oficiais do Granite. Exige que o modelo
  responda de verdade, que o filtro aceite e que a resposta não tenha lixo.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

CHECKS: list[str] = []


def check(condition: bool, message: str) -> None:
    status = "ok" if condition else "FALHOU"
    print(f"  [{status}] {message}", flush=True)
    if not condition:
        raise SystemExit(f"verificação falhou: {message}")
    CHECKS.append(message)


def annotate(message: str) -> None:
    """Emite anotação quando roda dentro do GitHub Actions."""
    if os.environ.get("GITHUB_ACTIONS") == "true":
        single_line = message.replace("\r", " ").replace("\n", " ")[:800]
        print(f"::error title=Granite::{single_line}", flush=True)


def http_json(url: str, payload: dict | None = None, timeout: float = 300.0) -> dict:
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    headers = {"Content-Type": "application/json"} if data is not None else {}
    request = urllib.request.Request(url, data=data, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8", "replace"))


def http_text(url: str, timeout: float = 10.0) -> str:
    """Lê um endpoint de texto simples (o /health do Cortex responde `ok`)."""
    with urllib.request.urlopen(urllib.request.Request(url), timeout=timeout) as response:
        return response.read().decode("utf-8", "replace").strip()


def wait_for(description: str, timeout: float, probe):
    deadline = time.time() + timeout
    last_error: object = None
    while time.time() < deadline:
        try:
            result = probe()
            if result:
                return result
        except Exception as error:  # servidor ainda subindo
            last_error = error
        time.sleep(0.5)
    raise SystemExit(f"tempo esgotado esperando {description}: {last_error}")


def port_is_open(port: int, host: str = "127.0.0.1") -> bool:
    import socket

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.settimeout(0.5)
        return probe.connect_ex((host, port)) == 0


def run(command: list[str], description: str, cwd: pathlib.Path) -> str:
    print(f"$ {' '.join(command)}", flush=True)
    completed = subprocess.run(command, cwd=str(cwd), capture_output=True, text=True)
    if completed.returncode != 0:
        output = (completed.stdout + completed.stderr).strip()
        annotate(f"{description}: {output[-600:]}")
        raise SystemExit(f"{description} falhou ({completed.returncode})")
    return completed.stdout


def prepare_state(cortex: pathlib.Path, state: pathlib.Path, corpus: pathlib.Path,
                  cwd: pathlib.Path) -> None:
    if (state / "tokenizer.cbpe").exists():
        print(f"estado já preparado em {state}", flush=True)
        return
    run([str(cortex), "train", "--corpus", str(corpus), "--state", str(state),
         "--epochs", "1"], "treino do estado inicial", cwd)


def main() -> int:
    parser = argparse.ArgumentParser(description="Integração Granite do Cortex")
    parser.add_argument("--cortex", required=True, type=pathlib.Path)
    parser.add_argument("--llama-server", required=True, type=pathlib.Path)
    parser.add_argument("--model", required=True, type=pathlib.Path)
    parser.add_argument("--state", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--corpus", type=pathlib.Path, default=None)
    parser.add_argument("--expect", choices=("fixture", "real"), default="fixture")
    parser.add_argument("--power", default="medium")
    parser.add_argument("--cortex-port", type=int, default=18080)
    parser.add_argument("--model-port", type=int, default=18081)
    parser.add_argument("--startup-timeout", type=float, default=420.0)
    arguments = parser.parse_args()

    # O Cortex roda com diretório de trabalho próprio; caminhos relativos
    # precisam ser resolvidos antes de virar linha de comando.
    for attribute in ("cortex", "llama_server", "model", "state", "work"):
        setattr(arguments, attribute, getattr(arguments, attribute).resolve())
    if arguments.corpus is not None:
        arguments.corpus = arguments.corpus.resolve()
    if not arguments.cortex.exists():
        annotate(f"binário do Cortex não encontrado: {arguments.cortex}")
        raise SystemExit(f"binário do Cortex não encontrado: {arguments.cortex}")
    if not arguments.model.exists():
        annotate(f"GGUF não encontrado: {arguments.model}")
        raise SystemExit(f"GGUF não encontrado: {arguments.model}")

    work = arguments.work
    work.mkdir(parents=True, exist_ok=True)
    log_path = work / "cortex-serve.log"
    corpus = arguments.corpus
    if corpus is None:
        corpus = pathlib.Path(__file__).resolve().parent.parent / "data" / "corpus.pt.txt"

    print("== preparando estado ==", flush=True)
    prepare_state(arguments.cortex, arguments.state, corpus,
                  pathlib.Path(__file__).resolve().parent.parent)

    environment = dict(os.environ)
    environment["CORTEX_LLAMA_SERVER"] = str(arguments.llama_server)
    environment["CORTEX_CURL_PATH"] = environment.get("CORTEX_CURL_PATH", "")
    command = [
        str(arguments.cortex), "serve",
        "--state", str(arguments.state),
        "--host", "127.0.0.1",
        "--port", str(arguments.cortex_port),
        "--power", arguments.power,
        "--no-internet",
        "--local-model", str(arguments.model),
        "--local-port", str(arguments.model_port),
        "--local-alias", "cortex-granite-ci",
    ]
    print(f"$ {' '.join(command)}", flush=True)
    log = log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(command, cwd=str(work), env=environment,
                               stdout=log, stderr=subprocess.STDOUT, text=True)
    try:
        base = f"http://127.0.0.1:{arguments.cortex_port}"
        model_base = f"http://127.0.0.1:{arguments.model_port}"

        wait_for("health do Cortex", 90.0,
                 lambda: http_text(base + "/health", timeout=5.0) == "ok")

        # O modelo é iniciado pelo próprio Cortex: se este /health responde, a
        # supervisão do processo filho funcionou de verdade.
        wait_for("llama-server iniciado pelo Cortex", arguments.startup_timeout,
                 lambda: http_json(model_base + "/health", timeout=5.0).get("status") == "ok")

        status = wait_for(
            "status com modelo local pronto", 120.0,
            lambda: (lambda value: value if value.get("language_api_state") == "ready"
                     and value.get("language_provider") == "local-open-model" else None)(
                http_json(base + "/api/status", timeout=10.0)))
        check(status.get("language_provider") == "local-open-model",
              "Cortex reporta provedor local-open-model")
        check(status.get("language_model") == arguments.model.name,
              f"Cortex reporta o GGUF exato do teste ({arguments.model.name})")
        check(int(status.get("language_context", 0)) >= 512,
              "Cortex negocia contexto com o servidor do modelo")

        models = http_json(model_base + "/v1/models", timeout=10.0)
        identifiers = [entry.get("id", "") for entry in models.get("data", [])]
        check("cortex-granite-ci" in identifiers,
              "llama-server responde com o alias configurado pelo Cortex")

        # Template Granite de verdade: o marcador de esforço precisa aparecer no
        # prompt apenas quando o Cortex pede raciocínio de baixo esforço.
        low_effort = http_json(model_base + "/apply-template", timeout=20.0, payload={
            "messages": [{"role": "user", "content": "teste"}],
            "chat_template_kwargs": {"enable_thinking": True, "low_effort": True},
        }).get("prompt", "")
        no_thinking = http_json(model_base + "/apply-template", timeout=20.0, payload={
            "messages": [{"role": "user", "content": "teste"}],
            "chat_template_kwargs": {"enable_thinking": False},
        }).get("prompt", "")
        check("<|start_of_role|>assistant<|end_of_role|>" in low_effort
              and "reasoning effort: low" in low_effort,
              "template aplica low_effort quando o Cortex pede Medium")
        check("reasoning effort: low" not in no_thinking
              and "<|start_of_role|>assistant<|end_of_role|>" in no_thinking,
              "template omite raciocínio quando o Cortex pede Flash")

        direct = http_json(model_base + "/v1/chat/completions", timeout=300.0, payload={
            "model": "cortex-granite-ci",
            "messages": [{"role": "user", "content": "responda algo curto"}],
            "max_tokens": 12,
            "temperature": 1.0,
            "top_p": 0.95,
            "chat_template_kwargs": {"enable_thinking": False},
        })
        direct_text = direct["choices"][0]["message"].get("content", "")
        check(len(direct_text) > 0, "modelo gera texto pelo endpoint OpenAI-compatible")

        def chat(power: str) -> dict:
            return http_json(base + "/api/chat", timeout=900.0, payload={
                "message": "Escreva um parágrafo curto sobre cooperação entre pessoas",
                "power": power,
            })

        reply = chat(arguments.power)
        check(reply.get("ok") is True, "API de chat responde com sucesso")
        text = reply.get("reply", "")
        check(isinstance(text, str) and len(text) > 0, "API de chat devolve texto não vazio")

        if arguments.expect == "fixture":
            # Pesos aleatórios não produzem texto útil: o correto é o filtro
            # rejeitar e a interface explicar que o modelo local respondeu.
            rejected = bool(reply.get("model_response_rejected"))
            used_model = reply.get("provider") == "local-open-model"
            check(rejected or used_model,
                  "resposta do GGUF de teste entrou no pipeline (aceita ou rejeitada pelo filtro)")
            warning = reply.get("warning", "")
            if rejected:
                check("modelo local ativo" in warning or "filtro nativo" in warning,
                      "aviso explica que o modelo local respondeu e foi rejeitado")
        else:
            # Com os pesos oficiais, os três modos de potência precisam produzir
            # resposta utilizável: é isso que valida o mapeamento
            # Flash/Medium/High para non-thinking/low-effort/thinking completo.
            for power in ("flash", "medium", "high"):
                answer = reply if power == arguments.power else chat(power)
                answer_text = answer.get("reply", "")
                check(answer.get("provider") == "local-open-model",
                      f"potência {power}: resposta vem do Granite ({answer.get('provider')})")
                check(not answer.get("model_response_rejected"),
                      f"potência {power}: filtro de qualidade aceita a resposta")
                check(int(answer.get("generated_tokens", 0)) > 0,
                      f"potência {power}: telemetria registra tokens gerados")
                check(len(answer_text) > 40, f"potência {power}: resposta tem conteúdo substancial")
                check(all(character.isprintable() or character in "\n\r\t"
                          for character in answer_text),
                      f"potência {power}: resposta sem bytes de controle")

        with log_path.open(encoding="utf-8", errors="replace") as captured:
            log_text = captured.read()
        check("model loaded" in log_text or "chat completion" in log_text,
              "log do llama-server confirma carga e atendimento pelo processo filho")

        print("== encerrando o Cortex ==", flush=True)
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=15)
        closed = wait_for("porta do modelo liberada após o encerramento", 30.0,
                          lambda: not port_is_open(arguments.model_port))
        check(bool(closed), "Cortex encerra o llama-server junto com ele")
    finally:
        if process.poll() is None:
            process.kill()
        log.close()

    print(f"\nOK — {len(CHECKS)} verificações de integração passaram.", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SystemExit as exit_code:
        if exit_code.code not in (0, None):
            print(f"falha na integração Granite (código {exit_code.code})", file=sys.stderr)
        raise
