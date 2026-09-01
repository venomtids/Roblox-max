# Cortex Native AI

Motor de linguagem neural e agente cognitivo **100% C++20**, criado do zero e sem Python, Node.js, npm, bibliotecas de deep learning ou serviços externos. O projeto compila para um único executável nativo; a biblioteca padrão de C++ é a única dependência de código. A interface de conversa é servida pelo próprio executável e usa somente HTML/CSS e JavaScript de navegador embutidos — não existe runtime, pacote ou build Node.js.

> O código implementa um motor completo e treinável. A qualidade linguística do checkpoint depende do corpus, das dimensões e do tempo de treino. O corpus pequeno em `data/` demonstra todo o pipeline, mas não pretende ter o conhecimento de um LLM pré-treinado em bilhões de tokens.

## 1. Arquitetura

```text
texto UTF-8
    │
    ▼
┌──────────────────────────────┐
│ Ingestão / BPE byte-level    │  aprende merges, encode/decode reversível
└──────────────┬───────────────┘
               │ TokenId[]
               ▼
┌──────────────────────────────┐
│ Núcleo neural GRU empilhado  │  forward, softmax, BPTT, clipping, Adam
│ + amostrador autoregressivo  │  top-k, temperatura, repetição, seed
└───────┬──────────────────────┘
        │ estado oculto normalizado
        ▼
┌──────────────────────────────┐
│ Memória vetorial persistente │  cosseno paralelo, ids, remoção, arquivo atômico
└───────┬──────────────────────┘
        │ episódios recuperados
        ▼
┌──────────────────────────────┐
│ Agente cognitivo             │  recuperar → contextualizar → inferir → consolidar
└──────────────────────────────┘
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

O `CognitiveAgent` executa um ciclo RAG local:

1. vetoriza a pergunta;
2. recupera episódios semanticamente próximos;
3. monta um contexto limitado;
4. gera a resposta no modelo neural;
5. recorta qualquer continuação de corpus (`Usuário:`, `Sistema:` ou novo turno), entregando apenas a resposta atual;
6. opcionalmente consolida pergunta e resposta como um novo episódio.

Essa finalização é importante em modelos autoregressivos: sem ela, o modelo pode responder corretamente e continuar prevendo o próximo diálogo do corpus, parecendo pouco direto.

### D. Interface de conversa no navegador

O comando `serve` inicia um servidor HTTP/1.1 escrito com sockets nativos em C++, ligado por padrão a `0.0.0.0:8080`. Ele entrega uma interface responsiva de chat, mantém as últimas mensagens no contexto curto e usa a memória vetorial para lembranças de longo prazo. HTML, CSS e JavaScript de navegador estão incorporados em `web_ui.cpp`; não há CDN, npm, Node.js ou arquivos web para instalar.

Endpoints locais:

- `GET /` — interface completa de conversa;
- `GET /api/status` — dimensões do modelo e quantidade de memórias;
- `POST /api/chat` — inferência, recuperação e consolidação;
- `POST /api/remember` — armazenamento explícito de um episódio;
- `GET /health` — health check simples.

O parser HTTP limita cabeçalhos/corpos, exige `Content-Length`, rejeita transfer encoding e valida o JSON sem biblioteca externa. Um pool separado atende conexões para não bloquear o pool matemático. A resposta inclui CSP, `nosniff`, conteúdo UTF-8 sanitizado e nunca habilita CORS para origens externas.

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
│   ├── core.hpp                      matriz, parâmetros, pool, PRNG e I/O
│   ├── memory.hpp                    contrato do vector store persistente
│   ├── neural_model.hpp              GRU, treino, inferência e geração
│   ├── tokenizer.hpp                 BPE byte-level
│   └── web_server.hpp                contrato da interface HTTP nativa
├── src/
│   ├── agent.cpp                     recuperação + contexto + consolidação
│   ├── core.cpp                      álgebra e infraestrutura nativa
│   ├── main.cpp                      CLI e validação de argumentos
│   ├── memory.cpp                    cosseno paralelo e serialização
│   ├── neural_model.cpp              forward, BPTT, Adam e sampling
│   ├── tokenizer.cpp                 treino/encode/decode BPE
│   ├── web_server.cpp                sockets, HTTP, JSON e API de chat
│   └── web_ui.cpp                    interface responsiva embutida
└── tests/
    └── test_main.cpp                 32 verificações determinísticas
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
  src/agent.cpp src/web_server.cpp src/web_ui.cpp src/main.cpp -o cortex
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
  --merges 128 \
  --embedding 64 \
  --hidden 128 \
  --layers 2 \
  --epochs 30 \
  --sequence 64 \
  --learning-rate 0.003
```

Os 30 ciclos padrão são deliberados: com apenas poucas épocas o pipeline já funciona, mas o modelo demonstrativo ainda produz fragmentos em vez de respostas estáveis. Para uso real, acompanhe a loss em validação e treine com um corpus muito maior.

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

Abra [http://localhost:8080](http://localhost:8080). A página permite conversar, iniciar uma nova conversa, ajustar temperatura/top-k/comprimento e ativar ou desativar a consolidação automática na memória. As dez mensagens mais recentes formam o contexto curto no navegador; episódios relevantes são recuperados da memória persistente pelo servidor.

Não execute esse comando diretamente na internet sem colocar autenticação, TLS, rate limiting e um proxy reverso à frente. O servidor embutido foi projetado para uso local, rede confiável e integração em desenvolvimento.

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
- presença e endpoints relativos da interface web embutida;
- smoke tests HTTP para página, status, health, chat, memória e JSON inválido.

## 8. Escala e evolução

A arquitetura é funcional, modular e adequada para experimentação nativa. Para uma implantação de grande escala:

- treine com corpus licenciado, maior e separado em treino/validação;
- substitua a busca exata por HNSW ou IVF quando houver milhões de episódios;
- use blocos quantizados/SIMD explícito para modelos maiores;
- faça batching de múltiplas sessões;
- adicione autenticação e limites antes de expor uma API de rede;
- meça qualidade, perplexidade, latência p50/p95/p99 e consumo de memória no hardware alvo.

A API HTTP embutida é intencionalmente pequena e sem dependências. Para exposição pública, mantenha `CognitiveAgent` como núcleo e coloque a aplicação atrás de um gateway autenticado ou substitua a camada de sockets por Crow/Drogon, preservando tokenizador, modelo e memória.
