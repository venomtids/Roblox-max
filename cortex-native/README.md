# Cortex Native AI

Agente cognitivo, servidor e motor de autoevolução **100% C++20**, sem Python, Node.js, npm, Docker ou bibliotecas fechadas. Sem configurar chave ou provedor, o núcleo local combina BPE, GRU/BPTT/Adam, memória, **70 tópicos curados**, análise de textos, filtro universal, pesquisa HTTPS com fontes, calculadora, benchmark real e o **Cortex Native Reasoner**. Com `--billion`, o mesmo executável supervisiona [llama.cpp](https://github.com/ggml-org/llama.cpp) e ativa **Qwen3 1.7B**; perfis maiores chegam a 30,5B. A interface usa somente JavaScript de navegador embutido, sem runtime ou build Node.

> **Nenhuma API ou chave é necessária.** O GRU auxiliar de aproximadamente 1 milhão de parâmetros não possui conhecimento geral suficiente para ser usado sem controle. Respostas do GRU, GGUF ou API atravessam o mesmo filtro de relevância, coerência, repetição e vazamento de papéis; uma saída degradada cai para o reasoner nativo. Para conhecimento amplo offline, use o modelo aberto pré-treinado de **1,7B parâmetros funcionais** ou um dos perfis de 0,6B, 4B, 14,8B e 30,5B. Isso não infla artificialmente o GRU nem afirma equivalência falsa a modelos de fronteira.

## 1. Arquitetura

```text
pergunta UTF-8 + modo Flash/Medium/High
    │
    ├──► BPE + GRU próprio ──► embedding ──► memória vetorial persistente
    ├──► base curada ─────────► recuperação lexical auditável
    ├──► analisador ──────────► estrutura/coerência/repetição/tópicos
    ├──► DuckDuckGo ──fallback Wikipedia──► fonte HTTPS + memória [WEB]
    │
    └──► contexto limitado pelo perfil de potência
             │
             ├──► Native Reasoner + cálculo/benchmark/memória
             ├──► Qwen3 1.7B+ GGUF local via llama.cpp C++
             ├──► GRU 1M auxiliar, somente quando sua saída é aceitável
             └──► API remota, apenas por opção explícita
                         │
                         ▼
              filtro universal de resposta
          relevância + coerência + repetição + papéis
                         │
             ┌───────────┴───────────┐
             │ aceitou               │ rejeitou
             ▼                       ▼
       revisão High opcional   fallback nativo seguro
             └───────────┬───────────┘
                         ▼
          telemetria + memória + autoanálise
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

1. classifica intenções verificáveis e comandos explícitos de análise;
2. executa benchmark, cálculo ou consulta de memória diretamente;
3. consulta 70 entradas curadas sobre programação, redes, segurança, ciência, saúde, história, economia e escrita;
4. vetoriza a pergunta e recupera episódios semanticamente próximos, inclusive conhecimento `[WEB]`;
5. somente sem evidência, monta contexto limitado pelo modo Flash/Medium/High e chama GGUF ou GRU;
6. recorta novos papéis (`Usuário:`, `Sistema:` etc.) e mede relevância, coerência, diversidade e repetição;
7. descarta saudações genéricas, evasão temática, loops e metaconversa degradada, inclusive do GGUF/API;
8. no modo High, executa uma segunda passagem que revisa fatos, instrução e clareza;
9. registra telemetria agregada de qualidade e opcionalmente consolida a resposta na memória.

Assim, `benchmark` executa uma medição real em vez de completar a palavra com uma saudação. Quando nem a base verificável nem o GRU possuem evidência, Cortex informa a limitação e sugere o GGUF local, sem pedir API.

### D. Raciocínio nativo confiável

`reasoner.cpp` normaliza português/UTF-8, classifica intenções por palavras inteiras e oferece respostas auditáveis sobre a própria arquitetura. `knowledge.cpp` mantém fatos explicativos estáveis e rastreáveis no próprio fonte. `quality.cpp` mede palavras, frases, parágrafos, legibilidade heurística, diversidade lexical, repetição, tópicos e sinais de baixa qualidade. Os comandos `analise este texto: ...` e `analise sua resposta` funcionam sem modelo generativo.

A camada inclui parser aritmético recursivo com precedência e proteção contra divisão por zero, benchmark do `forward` realmente carregado e consulta às memórias recuperadas. `BehaviorMonitor` agrega quantidade, rejeições, revisões e médias de qualidade sem armazenar o texto analisado. As métricas são sinais estruturais — não uma prova automática de veracidade — e a Cortex declara esse limite.

### E. Inferência aberta local para uso diário

`LocalModelServer` inicia `llama-server` diretamente com `fork/exec` no Linux/macOS ou `CreateProcess` no Windows, sempre sem shell. Ele restringe a API a `127.0.0.1`, espera `/health` e `/v1/models`, supervisiona o processo e o encerra junto com Cortex. O primeiro uso de `-hf` baixa o GGUF para o cache do usuário; depois disso, a inferência funciona offline.

O comando `--local` mede a RAM e escolhe um perfil conservador. `--billion` seleciona explicitamente Qwen3 1.7B Q8, com contexto de 4.096 tokens, recomendação de 4 GiB de RAM e nenhuma chave. Os pesos não são somados ao GRU: Qwen passa a ser o **gerador ativo**, enquanto o núcleo próprio continua fornecendo memória, RAG, internet, segurança e fallback. Isso eleva a capacidade funcional para bilhões de parâmetros sem tentar retreinar ou inflar o GRU.

### F. Modos de potência e filtro universal

| Modo | Saída máxima | Memórias | Contexto do agente | Revisão |
|---|---:|---:|---:|---|
| `flash` | 256 tokens | 3 | 512 tokens | não |
| `medium` | 1.024 tokens | 6 | 2.048 tokens | não |
| `high` | 4.096 tokens | 12 | 4.096 tokens | segunda passagem |

O limite final nunca excede a janela segura do GGUF. Cada chamada usa parâmetros imutáveis locais, portanto sessões concorrentes não alteram a configuração global do agente. O pool HTTP permanece separado do pool matemático; a cópia do monitor e a memória usam locks curtos.

O mesmo `assess_response` avalia todos os provedores. Se GGUF/API falhar na qualidade ou devolver metaconversa, a saída não chega ao usuário: o servidor tenta o reasoner nativo e depois o fallback explícito. O JSON informa `quality_score`, `relevance_score`, `coherence_score`, problemas, modo, revisão e rejeição do provedor.

### G. Interface de conversa no navegador

O comando `serve` inicia um servidor HTTP/1.1 escrito com sockets nativos em C++, ligado por padrão a `0.0.0.0:8080`. Ele entrega uma interface responsiva de chat, mantém as últimas mensagens no contexto curto e usa a memória vetorial para lembranças de longo prazo. HTML, CSS e JavaScript de navegador estão incorporados em `web_ui.cpp`; não há CDN, npm, Node.js ou arquivos web para instalar.

Endpoints locais:

- `GET /` — interface completa de conversa;
- `GET /api/status` — dimensões do modelo e quantidade de memórias;
- `POST /api/chat` — inferência, recuperação, filtro, potência e consolidação;
- `POST /api/analyze` — métricas determinísticas de qualquer texto;
- `POST /api/remember` — armazenamento explícito de um episódio;
- `POST /api/code/audit` — auditoria estática e plano sugerido;
- `POST /api/code/propose` — proposta direta revisável (compatibilidade);
- `POST /api/code/refine` — auditoria + proposta GGUF de autorrefinamento;
- `POST /api/code/apply` — aplicação confirmada, atômica e com backup;
- `POST /api/code/rollback` — restauração confirmada do backup registrado;
- `POST /api/validation/stage` — mostra e registra um comando livre para autorização;
- `POST /api/validation/run` — executa uma única vez após `EXECUTAR ID`;
- `GET /health` — health check simples.

O parser HTTP limita cabeçalhos/corpos, exige `Content-Length`, rejeita transfer encoding e valida o JSON sem biblioteca externa. Um pool separado atende conexões para não bloquear o pool matemático. Cada resposta informa provedor, potência e métricas de qualidade; pesquisas acrescentam fonte e `learned_from_web`. `/api/status` expõe tamanho da base curada e somente telemetria comportamental agregada. A resposta inclui CSP, `nosniff`, conteúdo UTF-8 sanitizado e nunca habilita CORS para origens externas.

### H. Conectividade opcional e internet

`LanguageApiClient` usa o protocolo OpenAI-compatible para o Qwen local e preserva compatibilidade com provedores remotos. O modelo GGUF em loopback não precisa de chave. Provedores externos são **opt-in explícito**: até a presença isolada de `GEMINI_API_KEY` é ignorada sem `--gemini`. Nenhum segredo é aceito pela UI, argumento ou fonte; quando usado, Bearer/corpo segue por pipe `curl --config -`, sem aparecer no argv ou em arquivo temporário. Em qualquer falha, o sistema aciona o resultado web já obtido ou o Native Reasoner com filtro GRU.

`InternetSearch` não requer chave: consulta por HTTPS o Instant Answer do DuckDuckGo e, quando ele não traz conteúdo, usa a API pública da Wikipedia em português como fallback. O comando **`pesquise na internet por <assunto>`** força a pesquisa mesmo com a opção automática da interface desligada. Um resultado objetivo responde diretamente ou fundamenta o modelo aberto; a resposta identifica o nome e a URL HTTPS da fonte.

Toda pesquisa bem-sucedida é consolidada imediatamente na memória persistente com o marcador `[WEB]`, inclusive quando a memória automática de conversas está desligada. Em perguntas posteriores, o reasoner exige sobreposição lexical, recupera esse registro e o identifica como `local-web-knowledge`; assim o fato pesquisado pode ser reutilizado sem nova conexão. As URLs de consulta são fixas, consultas passam por percent-encoding, respostas possuem limites estritos e referências sem HTTPS são descartadas. Isso não é um navegador irrestrito e reduz a superfície de SSRF.

### I. Autoevolução supervisionada

Há dois níveis explícitos:

- `--enable-self-edit`: modo controlado, restrito aos fontes existentes em `src/`, `include/`, `tests/` e `data/`;
- `--unrestricted-self-edit`: modo de sistema supervisionado, aceita caminho absoluto, substitui qualquer arquivo regular autorizado pela conta e cria arquivos/diretórios novos.

O modo irrestrito só inicia em `127.0.0.1`. Isso evita transformar uma porta de rede em execução remota, mas não cria uma allowlist de arquivos: a autoridade real passa a ser a conta do processo e, no Windows, o UAC. O fluxo é:

1. o GGUF recebe o caminho, conteúdo atual (ou indicação de arquivo novo) e objetivo;
2. a interface mostra destino, operação `CRIAR`/`SUBSTITUIR`, risco e conteúdo completo;
3. `APLICAR ID` concede permissão de uso único;
4. arquivos existentes recebem backup; arquivos novos são registrados para remoção no rollback;
5. hash detecta mudanças concorrentes e a publicação usa substituição atômica;
6. o manifesto em `.cortex-backups/` mantém `REVERTER ID` funcional após recompilar/reiniciar;
7. qualquer comando de validação pode ser preparado, exibido literalmente e executado uma vez somente depois de `EXECUTAR ID`; saída, exit code e log ficam visíveis.

Não há restrição de diretório, extensão ou conteúdo no modo irrestrito. Continuam existindo apenas controles de consentimento e integridade: nada é escrito ou executado silenciosamente. Uma conta sem permissão administrativa continuará recebendo `access denied` do sistema operacional.

### Por que C++ supera Python/Node neste caminho crítico

Neste cenário específico, C++20 oferece matrizes row-major contíguas, controle explícito das alocações, autovetorização/SIMD pelo compilador, threads nativas e ausência de interpretador/JIT/garbage collector no hot path. Isso reduz overhead e torna a latência mais previsível. Python pode orquestrar kernels nativos, mas foi deliberadamente excluído; aqui até tokenização, gradientes e persistência são nativos e inspecionáveis.

## 2. Estrutura de arquivos

```text
cortex-native/
├── ci/cortex-windows.yml.example       workflow MinGW-w64 pronto para ativação
├── Makefile                            build Linux/macOS/MinGW
├── README.md                           arquitetura e uso completo
├── WINDOWS.md                          instalação e operação Windows
├── build-windows.cmd                   compila, testa e gera cortex.exe
├── start-windows.cmd                   primeiro uso + Qwen3 1.7B
├── start-windows-system.cmd            UAC + automodificação de sistema
├── data/corpus.pt.txt                  corpus demonstrativo expandido
├── include/cortex/
│   ├── agent.hpp                       ciclo cognitivo + PowerMode
│   ├── connectivity.hpp                HTTPS, API linguística e busca web
│   ├── core.hpp                        matriz, pool, PRNG e I/O
│   ├── knowledge.hpp                   contrato da base curada
│   ├── local_model.hpp                 perfis GGUF e supervisor llama.cpp
│   ├── memory.hpp                      vector store persistente
│   ├── neural_model.hpp                GRU, treino, inferência e geração
│   ├── quality.hpp                     análise, filtro e BehaviorMonitor
│   ├── reasoner.hpp                    respostas locais verificáveis
│   ├── self_editor.hpp                 arquivos, permissões, comandos e rollback
│   ├── tokenizer.hpp                   BPE byte-level
│   └── web_server.hpp                  API HTTP nativa
├── src/
│   ├── agent.cpp                       recuperação, contexto e consolidação
│   ├── connectivity.cpp                curl e protocolos externos
│   ├── core.cpp                        álgebra e infraestrutura nativa
│   ├── knowledge.cpp                   70 tópicos + ranking lexical
│   ├── local_model.cpp                 supervisor do llama-server
│   ├── main.cpp                        CLI, doctor, analyze e serve
│   ├── memory.cpp                      cosseno paralelo e serialização
│   ├── neural_model.cpp                forward, BPTT, Adam e sampling
│   ├── quality.cpp                     métricas e filtro universal
│   ├── reasoner.cpp                    intenções e ferramentas
│   ├── self_editor.cpp                 pipeline de autoevolução segura
│   ├── tokenizer.cpp                   treino/encode/decode BPE
│   ├── web_server.cpp                  sockets, rotas e orquestração
│   └── web_ui.cpp                      chat e laboratório embutidos
└── tests/test_main.cpp                 104 verificações e mocks nativos
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
  src/quality.cpp src/knowledge.cpp src/agent.cpp src/connectivity.cpp \
  src/local_model.cpp src/reasoner.cpp src/self_editor.cpp src/web_server.cpp \
  src/web_ui.cpp src/main.cpp -o cortex
```

No MinGW/Windows, acrescente `-lws2_32 -lshell32`; o `Makefile` detecta o Windows. Use `build-windows.cmd`; veja [WINDOWS.md](WINDOWS.md). Um workflow MinGW-w64 pronto para copiar para `.github/workflows/` está em `ci/cortex-windows.yml.example`.

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

Abra [http://localhost:8080](http://localhost:8080). A página permite conversar, iniciar uma nova conversa, escolher **Flash, Medium ou Alta potência**, inspecionar qualidade e ativar a consolidação persistente quando ela for desejada. Digite, por exemplo, **`pesquise na internet por Ada Lovelace`** para consultar uma fonte sem chave e guardar o resultado; depois, uma pergunta como **`quem foi Ada Lovelace?`** pode usar a cópia local. A memória automática começa desligada para não transformar saudações, benchmarks e testes em lembranças irrelevantes, mas toda pesquisa web bem-sucedida é sempre preservada como conhecimento. As dez mensagens mais recentes continuam formando o contexto curto no navegador.

Atalhos locais novos:

```bash
./build/cortex analyze --text "Texto complexo a analisar."
./build/cortex doctor --state state
./build/cortex serve --state state --billion --power medium
```

`analyze` mede estrutura e sinais de qualidade sem exigir estado ou modelo. `doctor` verifica RAM, estado, curl, llama.cpp e prontidão para 1,7B. `--power flash|medium|high` também vale para `ask` e `chat`.

Não execute o servidor diretamente na internet sem autenticação, TLS, rate limiting e proxy reverso. Ele foi projetado para uso local, rede confiável e integração em desenvolvimento.

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
./build/cortex serve --state state --host 0.0.0.0 --port 8080 --billion
# ou escolha automaticamente pela RAM:
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

Flash e Medium usam `/no_think` no Qwen3 para respostas rápidas. Alta potência solicita `/think`, amplia orçamento/contexto e executa uma revisão separada; raciocínio interno não é apresentado como fonte ou prova.

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

Se o provedor cair, expirar ou exceder o timeout, a resposta volta ao Native Reasoner com GRU filtrado e a interface mostra o aviso. A opção **Pesquisar quando necessário** é independente e usa DuckDuckGo com fallback Wikipedia; o comando explícito funciona mesmo sem marcá-la. Desative toda conectividade com `--no-internet`.

### Habilitar autoevolução controlada

Use apenas localmente e com o repositório sob controle de versão:

```bash
./build/cortex serve --state state --host 127.0.0.1 --port 8080 \
  --local --enable-self-edit --source-root .
```

O painel **Autoevolução supervisionada** aparecerá. Para acesso completo ao repositório, caminhos externos, criação de arquivos e comandos livres de validação, use somente no loopback:

```bash
./build/cortex serve --state state --host 127.0.0.1 --billion \
  --unrestricted-self-edit --source-root .
```

A proposta não é aplicada automaticamente: `APLICAR ID` autoriza uma escrita e `EXECUTAR ID` autoriza um comando. `REVERTER ID` funciona mesmo após reiniciar, desde que o alvo não tenha sido modificado novamente. Backups ficam em `.cortex-backups/`, logs em `.cortex-validation/` e nenhum deles entra no Git. No Windows, `start-windows-system.cmd` solicita UAC antes de iniciar esse modo.

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
- presença dos controles de modelo local, potência, internet e autoevolução na interface;
- filtro de respostas aceitáveis/degradadas e monitor comportamental;
- análise estrutural, recuperação curada de programação e conhecimento geral;
- payload OpenAI-compatible e amostragem do modelo validados com processo mock nativo;
- auditoria, proposta GGUF simulada, confirmação, backup e rollback após reinício;
- criação em caminho absoluto, remoção por rollback e comando de validação autorizado.

A suíte corrente executa **104 verificações** sem rede nem modelo externo. `make sanitize-test` recompila o mesmo conjunto com ASan/UBSan e detecção de leaks.

## 8. Escala e evolução

A arquitetura é funcional, modular e adequada para experimentação nativa. Para uma implantação de grande escala:

- treine com corpus licenciado, maior e separado em treino/validação;
- substitua a busca exata por HNSW ou IVF quando houver milhões de episódios;
- use blocos quantizados/SIMD explícito para modelos maiores;
- faça batching de múltiplas sessões;
- adicione autenticação e limites antes de expor uma API de rede;
- meça qualidade, perplexidade, latência p50/p95/p99 e consumo de memória no hardware alvo.

A API HTTP embutida é intencionalmente pequena e sem dependências. Para exposição pública, mantenha `CognitiveAgent` como núcleo e coloque a aplicação atrás de um gateway autenticado ou substitua a camada de sockets por Crow/Drogon, preservando tokenizador, modelo e memória.
