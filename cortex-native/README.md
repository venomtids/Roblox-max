# Cortex Native AI

Agente cognitivo e servidor **100% C++20**, sem Python, Node.js, npm, Docker ou bibliotecas fechadas. Sem configurar chave ou provedor, o núcleo local combina BPE, GRU/BPTT/Adam, memória, calculadora, benchmark real e o **Cortex Native Reasoner**, que reconhece intenções verificáveis e bloqueia continuações sem relação com a pergunta. Com `--local`, o mesmo executável supervisiona [llama.cpp](https://github.com/ggml-org/llama.cpp) e pesos GGUF do [Qwen3](https://huggingface.co/collections/Qwen/qwen3). A interface usa somente JavaScript de navegador embutido, sem runtime ou build Node.

> **Nenhuma API ou chave é necessária.** O GRU de 1.003.428 parâmetros não possui conhecimento geral suficiente para ser usado sem controle: uma saída fluente pode estar errada. Por isso, tarefas determinísticas passam pelo reasoner e toda saída neural restante precisa superar um filtro de relevância; caso contrário Cortex admite o limite em vez de inventar. Para conhecimento amplo offline, o gerador pode ser trocado por um modelo aberto pré-treinado de **0,6B, 1,7B, 4B, 14,8B ou 30,5B parâmetros**. Não se afirma equivalência falsa a modelos de fronteira.

## 1. Arquitetura

```text
pergunta UTF-8
    │
    ├──► BPE + GRU próprio ──► embedding ──► memória vetorial persistente
    │                                              │
    ├──► DuckDuckGo opcional ──► resumo + fonte ───┤
    │                                              ▼
    └──────────────────────────────► contexto enriquecido
                                                   │
                     ┌─────────────────────────────┼──────────────────────┐
                     ▼                             ▼                      ▼
          Native Reasoner + ferramentas     Qwen3 GGUF local      GRU 1M controlado
          benchmark/cálculo/memória          llama.cpp C++         filtro de relevância
                     │                             │                      │
                     └─────────────────────────────┴──────────────────────┘
                                                   ▼
                              resposta confiável + consolidação na memória
                                                   │
                                                   └── API remota somente se solicitada
```

### A. Ingestão e tokenização

`BpeTokenizer` começa com quatro tokens especiais e os 256 bytes possíveis. Durante o treino, conta pares adjacentes, escolhe deterministicamente o mais frequente e cria um novo token. Como a base são bytes, qualquer UTF-8 — inclusive acentos, emoji e NUL — é reversível sem ICU e sem vocabulário externo.

### B. Motor de inferência e aprendizado

O núcleo é uma linguagem neural GRU com quantidade configurável de camadas. Para cada token e camada, o código calcula explicitamente:

```text
zₜ = sigmoid(Wz·xₜ + Uz·hₜ₋₁ + bz)           gate de atualização
rₜ = sigmoid(Wr·xₜ + Ur·hₜ₋₁ + br)           gate de reset
nₜ = tanh(Wn·xₜ + Un·(rₜ ⊙ hₜ₋₁) + bn)       estado candidato
hₜ = (1-zₜ) ⊙ nₜ + zₜ ⊙ hₜ₋₁                novo estado
p  = softmax(Wo·hₜ + bo)                       próximo token
```

O treinamento não chama uma biblioteca neural. `neural_model.cpp` deriva e aplica manualmente os gradientes de cada gate em ordem temporal inversa (**BPTT truncado**), calcula o gradiente da entropia cruzada, faz clipping pela norma global e atualiza todos os parâmetros com **Adam**. Pesos e dois momentos do otimizador são persistidos, portanto o formato suporta continuação de treinamento.

A geração é autoregressiva e possui top-k, temperatura, penalidade de repetição, EOS e PRNG determinístico.

### C. Estado e memória de longo prazo

A média dos estados ocultos de uma frase forma um embedding L2-normalizado. `LongTermMemory` persiste texto, vetor, timestamp e id monotônico. A busca exata por similaridade de cosseno divide registros entre threads do pool persistente, sem locks no caminho interno; só há um `shared_lock` durante a consulta. Essa busca exata fornece um baseline correto antes de trocar por HNSW/IVF em coleções muito grandes.

O `CognitiveAgent` executa um ciclo local com confiança explícita:

1. classifica intenções que podem ser verificadas sem geração probabilística;
2. executa benchmark, cálculo ou consulta de memória diretamente quando solicitado;
3. para outros casos, vetoriza a pergunta e recupera episódios semanticamente próximos;
4. monta um contexto limitado e produz uma continuação no GRU;
5. recorta novos papéis (`Usuário:`, `Sistema:` etc.) e mede a sobreposição temática;
6. descarta saudações genéricas, troca de papel, repetição extrema e texto sem relação com a pergunta;
7. opcionalmente consolida pergunta e resposta como um novo episódio.

Assim, `benchmark` executa uma medição real em vez de completar a palavra com uma saudação. Quando nem a base verificável nem o GRU possuem evidência, Cortex informa a limitação e sugere o GGUF local, sem pedir API.

### D. Raciocínio nativo confiável

`reasoner.cpp` normaliza português/UTF-8, classifica intenções por palavras inteiras e oferece respostas auditáveis sobre a própria arquitetura. Ele inclui um parser aritmético recursivo com precedência, parênteses, potência e proteção contra divisão por zero; um benchmark que mede o `forward` do checkpoint realmente carregado; e consulta explícita às memórias recuperadas. A camada não finge conhecer fatos fora da base. O filtro neural exige termos relevantes da pergunta e rejeita as saudações genéricas que o corpus pequeno tende a produzir.

### E. Inferência aberta local para uso diário

`LocalModelServer` inicia `llama-server` diretamente com `fork/exec` no Linux/macOS ou `CreateProcess` no Windows, sempre sem shell. Ele restringe a API a `127.0.0.1`, espera `/health` e `/v1/models`, supervisiona o processo e o encerra junto com Cortex. O primeiro uso de `-hf` baixa o GGUF para o cache do usuário; depois disso, a inferência funciona offline.

O comando `--local` mede a RAM e escolhe um perfil conservador. Os pesos não são somados ao GRU: o Qwen passa a ser o **gerador ativo**, enquanto o núcleo próprio continua fornecendo memória, RAG, internet, segurança e fallback. Isso eleva a capacidade real de 1 milhão para bilhões de parâmetros sem tentar retreinar bilhões de pesos no computador do usuário.

### F. Interface de conversa no navegador

O comando `serve` inicia um servidor HTTP/1.1 escrito com sockets nativos em C++, ligado por padrão a `0.0.0.0:8080`. Ele entrega uma interface responsiva de chat, mantém as últimas mensagens no contexto curto e usa a memória vetorial para lembranças de longo prazo. HTML, CSS e JavaScript de navegador estão incorporados em `web_ui.cpp`; não há CDN, npm, Node.js ou arquivos web para instalar.

Endpoints locais:

- `GET /` — interface completa de conversa;
- `GET /api/status` — dimensões do modelo e quantidade de memórias;
- `POST /api/chat` — inferência, recuperação e consolidação;
- `POST /api/remember` — armazenamento explícito de um episódio;
- `POST /api/code/propose` — proposta revisável de mudança em um fonte;
- `POST /api/code/apply` — aplicação confirmada com backup;
- `GET /health` — health check simples.

O parser HTTP limita cabeçalhos/corpos, exige `Content-Length`, rejeita transfer encoding e valida o JSON sem biblioteca externa. Um pool separado atende conexões para não bloquear o pool matemático. A resposta inclui CSP, `nosniff`, conteúdo UTF-8 sanitizado e nunca habilita CORS para origens externas.

### G. Conectividade opcional e internet

`LanguageApiClient` usa o protocolo OpenAI-compatible para o Qwen local e preserva compatibilidade com provedores remotos. O modelo GGUF em loopback não precisa de chave. Provedores externos são **opt-in explícito**: até a presença isolada de `GEMINI_API_KEY` é ignorada sem `--gemini`. Nenhum segredo é aceito pela UI, argumento ou fonte; quando usado, Bearer/corpo segue por pipe `curl --config -`, sem aparecer no argv ou em arquivo temporário. Em qualquer falha, o sistema aciona o resultado web já obtido ou o Native Reasoner com filtro GRU.

`InternetSearch` consulta por HTTPS a API Instant Answer do DuckDuckGo quando o usuário ativa **Pesquisar na internet**. Um resultado objetivo pode responder diretamente ou enriquecer a API linguística. A URL é fixa, respostas têm limite e HTTP remoto sem TLS é rejeitado. Isso não é um navegador irrestrito e reduz a superfície de SSRF.

### H. Autoevolução controlada

O sistema pode propor e aplicar mudanças ao próprio fonte, mas **não recebe permissão irrestrita**. Com `--enable-self-edit`, a interface libera um laboratório em duas etapas:

1. o modelo GGUF local (ou um provedor opt-in) recebe um único arquivo permitido e propõe sua substituição completa;
2. a pessoa revisa todo o conteúdo e digita `APLICAR ID`;
3. o servidor verifica caminho, extensão, symlinks e hash contra alterações concorrentes;
4. cria backup em `.cortex-backups/` e publica por escrita atômica;
5. a mudança só entra no processo depois de `make test`, recompilação e reinício.

Arquivos fora de `src/`, `include/`, `tests/` e `data/`, caminhos relativos, scripts e executáveis são bloqueados. Autoedição deve ser habilitada somente em `127.0.0.1` ou em rede confiável.

### Por que C++ supera Python/Node neste caminho crítico

Neste cenário específico, C++20 oferece matrizes row-major contíguas, controle explícito das alocações, autovetorização/SIMD pelo compilador, threads nativas e ausência de interpretador/JIT/garbage collector no hot path. Isso reduz overhead e torna a latência mais previsível. Python pode orquestrar kernels nativos, mas foi deliberadamente excluído; aqui até tokenização, gradientes e persistência são nativos e inspecionáveis.

## 2. Estrutura de arquivos

```text
cortex-native/
├── Makefile
├── README.md
├── data/
│   └── corpus.pt.txt                 corpus demonstrativo em português
├── include/cortex/
│   ├── agent.hpp                     contrato do ciclo cognitivo
│   ├── connectivity.hpp              HTTPS, API linguística e busca web
│   ├── core.hpp                      matriz, parâmetros, pool, PRNG e I/O
│   ├── local_model.hpp               perfis GGUF e supervisor llama.cpp
│   ├── memory.hpp                    contrato do vector store persistente
│   ├── neural_model.hpp              GRU, treino, inferência e geração
│   ├── reasoner.hpp                  respostas locais verificáveis e filtro lógico
│   ├── self_editor.hpp               autoedição com revisão humana
│   ├── tokenizer.hpp                 BPE byte-level
│   └── web_server.hpp                contrato da interface HTTP nativa
├── src/
│   ├── agent.cpp                     recuperação + contexto + consolidação
│   ├── connectivity.cpp              curl nativo e protocolos externos
│   ├── core.cpp                      álgebra e infraestrutura nativa
│   ├── local_model.cpp               launcher/supervisor nativo do llama-server
│   ├── main.cpp                      CLI e validação de argumentos
│   ├── memory.cpp                    cosseno paralelo e serialização
│   ├── neural_model.cpp              forward, BPTT, Adam e sampling
│   ├── reasoner.cpp                  intenções, cálculo, benchmark e relevância
│   ├── self_editor.cpp               proposta, hash, backup e aplicação
│   ├── tokenizer.cpp                 treino/encode/decode BPE
│   ├── web_server.cpp                sockets, HTTP, JSON e API de chat
│   └── web_ui.cpp                    interface responsiva embutida
└── tests/
    └── test_main.cpp                 verificações determinísticas e API mock
```

Os arquivos principais possuem comentários junto a cada bloco algorítmico e às linhas onde as equações são aplicadas. Operações triviais não são escondidas por macros ou frameworks; o fluxo matemático pode ser acompanhado diretamente no fonte.

## 3. Compilar e testar

Pré-requisitos: compilador C++20 com suporte a threads e `make`. Não há etapa de geração de código nem download de pacotes.

```bash
cd cortex-native
make test
make release
```

Compilação direta, caso `make` não esteja disponível:

```bash
g++ -Iinclude -std=c++20 -O3 -DNDEBUG -march=native -flto -pthread \
  src/core.cpp src/tokenizer.cpp src/neural_model.cpp src/memory.cpp \
  src/agent.cpp src/connectivity.cpp src/local_model.cpp src/reasoner.cpp \
  src/self_editor.cpp src/web_server.cpp src/web_ui.cpp src/main.cpp -o cortex
```

No MinGW/Windows, acrescente `-lws2_32`; o `Makefile` detecta o Windows e faz isso automaticamente.

Para depurar memória e comportamento indefinido:

```bash
make debug
./build/cortex-debug help
```

## 4. Treinar

```bash
./build/cortex train \
  --corpus data/corpus.pt.txt \
  --state state \
  --merges 160 \
  --embedding 256 \
  --hidden 256 \
  --layers 2 \
  --epochs 25 \
  --sequence 64 \
  --learning-rate 0.0025
```

Com o corpus incluído, essa configuração cria **1.003.428 parâmetros treináveis**. As 25 épocas padrão são deliberadas: com poucas épocas o pipeline funciona, mas ainda produz fragmentos. Esse checkpoint continua útil para embeddings, estudo e fallback; para conversas diárias, use `--local`, que ativa bilhões de parâmetros abertos já pré-treinados.

Arquivos produzidos:

```text
state/tokenizer.cbpe   vocabulário e ranks BPE
state/model.cgru       configuração, pesos e momentos Adam
state/memory.cmem      episódios de longo prazo
```

Um novo treinamento reinicia a memória porque os novos pesos definem outro espaço vetorial. Isso impede comparar embeddings produzidos por versões incompatíveis do modelo.

## 5. Conversar com a IA

Depois de treinar, inicie a interface:

```bash
./build/cortex serve --state state --host 0.0.0.0 --port 8080
```

Abra [http://localhost:8080](http://localhost:8080). A página permite conversar, iniciar uma nova conversa, ajustar temperatura/top-k/comprimento e ativar a consolidação persistente quando ela for desejada. A memória automática começa desligada para não transformar saudações, benchmarks e testes em lembranças irrelevantes. As dez mensagens mais recentes continuam formando o contexto curto no navegador.

Não execute esse comando diretamente na internet sem colocar autenticação, TLS, rate limiting e um proxy reverso à frente. O servidor embutido foi projetado para uso local, rede confiável e integração em desenvolvimento.

### Ativar o modelo aberto local — caminho recomendado

Instale o executável C++ do [llama.cpp](https://github.com/ggml-org/llama.cpp/blob/master/docs/install.md). No Windows 10/11:

```powershell
winget install --exact --id ggml.llamacpp
```

Em Linux/macOS, use um pacote nativo ou compile diretamente:

```bash
git clone --depth 1 https://github.com/ggml-org/llama.cpp.git
cmake -S llama.cpp -B llama.cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build llama.cpp/build --config Release -j --target llama-server
export CORTEX_LLAMA_SERVER="$PWD/llama.cpp/build/bin/llama-server"
```

Veja a recomendação para a máquina e depois inicie. A opção `--local` seleciona o maior perfil seguro para a RAM detectada, baixa os pesos oficiais na primeira execução e inicia tudo:

```bash
./build/cortex models
./build/cortex serve --state state --host 0.0.0.0 --port 8080 --local
```

| Perfil automático | RAM recomendada | Modelo ativo | Quantização/tamanho aproximado |
|---|---:|---|---:|
| `tiny` | 2–3 GiB | Qwen3 0.6B | Q8, 639 MB |
| `edge` | 4–7 GiB | Qwen3 1.7B | Q8, 1,83 GB |
| `daily` | 8–23 GiB | Qwen3 4B | Q4_K_M, 2,5 GB |
| `workstation` | 24–31 GiB | Qwen3 14.8B | Q4_K_M, 9 GB |
| `max` | 32+ GiB | Qwen3 30.5B-A3B MoE | Q4_K_M, 18,6 GB |

Escolha manualmente quando desejar priorizar velocidade ou qualidade:

```bash
./build/cortex serve --state state --local-profile daily
./build/cortex serve --state state --local-profile workstation
```

Também é possível usar qualquer GGUF compatível:

```bash
./build/cortex serve --state state \
  --local-model models/meu-modelo.gguf \
  --local-name "Meu modelo aberto"
```

Ou outra referência do Hugging Face sem Python:

```bash
./build/cortex serve --state state \
  --local-hf Qwen/Qwen3-4B-GGUF:Q4_K_M
```

Opções de desempenho: `--local-context`, `--local-threads`, `--local-gpu-layers`, `--local-port` e `--llama-server`. O padrão tenta descarregar todas as camadas possíveis na GPU e continua em CPU quando o backend não possui GPU. O servidor interno fica somente em loopback e a interface exibe o nome, a contagem real de parâmetros e a janela de contexto do perfil ativo.

O chat usa o modo `/no_think` do Qwen3 por padrão para respostas diárias rápidas e sem consumir a saída com raciocínio oculto. Inclua `/think` na pergunta quando quiser priorizar raciocínio aprofundado em matemática ou código.

Para máquinas comuns, 4B–14B já tornam o chat muito mais útil que o GRU demonstrativo. Modelos abertos com centenas de bilhões de parâmetros existem, mas exigem dezenas ou centenas de GiB de RAM/VRAM; combinar checkpoints menores não cria gratuitamente conhecimento equivalente a um modelo de fronteira.

### Compatibilidade Gemini — totalmente opcional

Esta seção não é necessária para o funcionamento da Cortex. Somente quem optar explicitamente por um provedor remoto deve criar uma chave e ativar `--gemini`; definir a variável sozinha não muda o modo local. Não cole a chave no chat, não a coloque na linha de comando e não a grave no repositório.

```bash
export GEMINI_API_KEY="SUA_CHAVE_NO_SEU_TERMINAL"
./build/cortex serve --state state --gemini
```

PowerShell:

```powershell
$env:GEMINI_API_KEY = "SUA_CHAVE_NO_SEU_TERMINAL"
.\build\cortex.exe serve --state state --gemini
```

O endpoint segue a compatibilidade OpenAI oficial; `--gemini` é sempre obrigatório para esse atalho. Os defaults são:

```text
endpoint: https://generativelanguage.googleapis.com/v1beta/openai/chat/completions
modelo:   gemini-3.7-flash
esforço:  medium
```

É possível trocar o modelo com `GEMINI_MODEL`, o esforço com `GEMINI_REASONING_EFFORT=low|medium|high`, ou pela CLI com `--llm-model` e `--llm-reasoning`. Se a chave estiver ausente, inválida ou a rede falhar, o Native Reasoner e o fallback local continuam atendendo e um aviso aparece no chat.

### Usar outra API OpenAI-compatible

Para outro provedor, mantenha a chave exclusivamente em uma variável de ambiente e informe apenas **o nome dessa variável** ao Cortex quando necessário:

```bash
export CORTEX_LLM_API_KEY="SUA_CHAVE_NO_SEU_TERMINAL"
./build/cortex serve --state state \
  --llm-url https://api.openai.com/v1/chat/completions \
  --llm-model SEU_MODELO
```

Também é possível usar Groq ou um servidor `llama.cpp` nativo local:

```bash
./build/cortex serve --state state \
  --llm-url http://127.0.0.1:8081/v1/chat/completions \
  --llm-model modelo-local
```

Se o provedor cair, expirar ou exceder o timeout, a resposta volta ao Native Reasoner com GRU filtrado e a interface mostra o aviso. O botão **Pesquisar na internet** é independente e usa DuckDuckGo; desative toda conectividade com `--no-internet`.

### Habilitar autoevolução controlada

Use apenas localmente e com o repositório sob controle de versão:

```bash
./build/cortex serve --state state --host 127.0.0.1 --port 8080 \
  --local --enable-self-edit --source-root .
```

O botão **Autoevolução controlada** aparecerá. O modelo nunca aplica uma proposta sozinho: revisão e confirmação `APLICAR ID` são obrigatórias. Em seguida execute `make test && make release` e reinicie. Backups ficam em `.cortex-backups/` e não entram no Git.

Geração neural direta pela CLI:

```bash
./build/cortex generate --state state \
  --prompt "Usuário: Como funciona uma GRU?\nAssistente:" \
  --tokens 100 --top-k 16 --temperature 0.7
```

Ensinar e recuperar um episódio sem retreinar:

```bash
./build/cortex remember --state state --text "O codinome do projeto é Aurora."
./build/cortex recall --state state --query "qual é o codinome?" --limit 3
```

Executar um ciclo completo do agente:

```bash
./build/cortex ask --state state \
  --prompt "Explique a memória de longo prazo" \
  --memory-hits 3 --context 512 --tokens 100 --show-memory
```

Chat persistente:

```bash
./build/cortex chat --state state
```

Comandos internos: `:lembrar TEXTO`, `:recordar CONSULTA` e `:sair`.

Inspeção e benchmark:

```bash
./build/cortex inspect --state state
./build/cortex benchmark --state state --steps 5000
```

## 6. Decisões de performance

- **Buffers contíguos:** `Matrix` e todos os parâmetros usam `std::vector<float>` row-major.
- **Sem alocação por peso:** gradiente e momentos Adam são reservados uma vez por parâmetro.
- **SIMD amigável:** os produtos escalares são loops simples sobre memória contígua, otimizáveis por `-O3 -march=native`.
- **Pool matemático persistente:** threads auxiliares vivem por todo o processo; produtos grandes e busca vetorial usam `parallel_for`.
- **Pool HTTP independente:** conexões web não bloqueiam o pool matemático e uma fila limitada aplica backpressure.
- **Chamador participa:** uma fatia de cada trabalho roda na thread solicitante, reduzindo espera e melhorando utilização.
- **BPTT truncado:** limita memória proporcionalmente a `sequence_length`, não ao corpus inteiro.
- **Move semantics:** caches, estados e resultados grandes são movidos em vez de copiados quando deixam uma etapa.
- **Formato binário:** elimina parsing textual de pesos e preserva cada `float` bit a bit.
- **Publicação segura:** checkpoints são escritos em arquivo temporário no mesmo diretório e então renomeados.

## 7. Validação e segurança de dados

O carregamento verifica magic/version, dimensões máximas, formas e nomes dos parâmetros, quantidade de registros, ids duplicados, tamanhos de texto e valores float não finitos. Entradas incompatíveis falham com uma mensagem explícita antes de alocações arbitrárias. Os testes cobrem:

- produto matricial normal e transposto;
- reversibilidade UTF-8 e persistência BPE;
- redução real de loss por BPTT/Adam;
- checkpoint bit a bit e amostragem determinística;
- ranking, persistência e remoção da memória vetorial;
- configuração neural própria próxima de um milhão de parâmetros;
- benchmark nativo, precedência aritmética, divisão por zero e consulta de memória;
- rejeição da saudação aleatória para a entrada `benchmark` e fallback sem invenção;
- perfis Qwen de 0,6B a 30,5B, seleção por RAM e argumentos seguros do launcher;
- percent-encoding, JSON Unicode e bloqueio de HTTP remoto sem TLS;
- presença dos controles de modelo local, API, internet e autoedição na interface;
- payload OpenAI-compatible e amostragem do modelo validados com processo mock nativo;
- fluxo de proposta, confirmação, backup e aplicação em uma árvore temporária.

## 8. Escala e evolução

A arquitetura é funcional, modular e adequada para experimentação nativa. Para uma implantação de grande escala:

- treine com corpus licenciado, maior e separado em treino/validação;
- substitua a busca exata por HNSW ou IVF quando houver milhões de episódios;
- use blocos quantizados/SIMD explícito para modelos maiores;
- faça batching de múltiplas sessões;
- adicione autenticação e limites antes de expor uma API de rede;
- meça qualidade, perplexidade, latência p50/p95/p99 e consumo de memória no hardware alvo.

A API HTTP embutida é intencionalmente pequena e sem dependências. Para exposição pública, mantenha `CognitiveAgent` como núcleo e coloque a aplicação atrás de um gateway autenticado ou substitua a camada de sockets por Crow/Drogon, preservando tokenizador, modelo e memória.
