# Cortex Native AI

Cortex é um assistente local híbrido escrito em **C++20**. O núcleo não usa Python, Node.js, npm, Docker ou framework neural de caixa-preta. Ele reúne ferramentas determinísticas, recuperação de conhecimento, memória persistente, uma malha de especialistas nativos e, no perfil principal para o Samsung X40, o modelo aberto **IBM Granite 4.2 3B GGUF** executado por `llama.cpp`.

- **Sem chave e sem API obrigatória:** o chat, os documentos, a memória, a pesquisa pública, a análise, o benchmark e o modelo GGUF funcionam localmente.
- **Mais de 1 bilhão de parâmetros funcionais:** Granite 4.2 3B é o gerador principal do perfil X40; o GRU próprio de aproximadamente 1 milhão de parâmetros é apenas um componente auxiliar.
- **Supervisão humana:** Cortex pode auditar, criar e modificar código, mas nunca grava ou executa uma validação sem uma confirmação de uso único.
- **Windows CPU-first:** o perfil X40 usa Q4_K_M, 4 threads, contexto de 4.096 tokens e `-ngl 0`.
- **Interface direta:** `cortex serve` abre um chat responsivo no navegador; o JavaScript é apenas código de navegador incorporado ao executável e não depende do ecossistema Node.

> Limite honesto: um Granite 3B quantizado é muito mais capaz que o GRU experimental, mas não é equivalente a um modelo de fronteira como Gemini. Treinar do zero um modelo dessa classe em um Core i5 com HD não é viável. A arquitetura aproxima a experiência por composição: pesos abertos + RAG + memória + ferramentas + especialistas + verificação.

## Arquitetura

```text
mensagem UTF-8 / documento / comando
                 │
                 ▼
        orquestrador C++20
                 │
      ┌──────────┼──────────────────┐
      │          │                  │
      ▼          ▼                  ▼
ferramentas   recuperação       classificador
nativas       híbrida            de intenção
      │          │                  │
 cálculo      70 temas              ├─ conversa e escrita criativa
 benchmark    documentos            ├─ artigo longo e resumo
 análise      memória                ├─ comparação e planejamento
 internet     pesquisa HTTPS         └─ programação C++20
      │          │                  │
      └──────────┴─────────┬────────┘
                           ▼
                    Cognitive Mesh
                           │
             ┌─────────────┴─────────────┐
             │                           │
             ▼                           ▼
 Granite 4.2 3B local             resposta nativa
 via llama.cpp/loopback           fundamentada/fallback
             │                           │
             └─────────────┬─────────────┘
                           ▼
                 filtro de qualidade
       relevância • coerência • repetição • papéis
                           │
                    memória persistente
```

### 1. Ingestão e tokenização

`BpeTokenizer` começa com tokens especiais e os 256 valores de byte. O treino aprende merges BPE deterministicamente; qualquer sequência UTF-8 permanece reversível. `cortex ingest` aceita um arquivo ou percorre um diretório recursivamente, ignora diretórios de build/controle de versão, rejeita conteúdo binário e divide texto em trechos com origem explícita.

A ingestão não altera pesos e por isso é rápida, incremental e auditável. Os trechos ficam na memória de longo prazo e entram no contexto apenas quando a consulta os recupera.

### 2. Inferência e processamento

O roteamento tem três níveis:

1. comandos verificáveis — cálculo, benchmark, consulta de memória e análise textual — não são enviados a um gerador;
2. pedidos de composição chegam ao Cognitive Mesh e ao Granite, em vez de serem interceptados por uma palavra genérica como “segurança” ou “concorrência”;
3. fatos sem evidência recebem um limite explícito ou pesquisa com fonte, não uma continuação aleatória do GRU.

O Granite roda em processo separado, iniciado e supervisionado por `LocalModelServer`. A comunicação usa `/v1/chat/completions` somente em `127.0.0.1`. No perfil X40:

```text
modelo:      ibm-granite/granite-4.2-3b-GGUF:Q4_K_M
parâmetros:  aproximadamente 3 bilhões
arquivo:     aproximadamente 2,24 GB
contexto:    4.096 tokens
threads:     4
GPU layers:  0 (CPU-only)
amostragem:  temperature 1.0, top_p 0.95
```

O controle oficial de raciocínio é mapeado assim:

| Potência | Granite | Saída máxima do agente | Memórias | Contexto |
|---|---|---:|---:|---:|
| Flash | thinking desligado | 256 tokens | 3 | 512 |
| Medium | low effort | 1.024 tokens | 6 | 2.048 |
| High | thinking completo + revisão | 4.096 tokens | 12 | 4.096 |

A janela efetiva nunca ultrapassa o perfil carregado. High melhora profundidade, mas usa mais RAM e tempo.

### 3. Cognitive Mesh

`native_cognition.cpp` implementa especialistas transparentes para:

- conversa aberta sem fingir sentimentos pessoais;
- narrativa e poesia;
- brainstorming por múltiplas perspectivas;
- artigo longo com introdução, seções fundamentadas, síntese, limites e conclusão;
- resumo extrativo;
- comparação e decisão multicritério;
- decomposição verificável e diagnóstico causal;
- geração de programas C++20 para contratos reconhecidos;
- síntese de várias evidências curadas, documentais, episódicas e web.

Essa camada permanece útil sem um GGUF e também serve como fallback quando o gerador falha. Ela não tenta se passar por um LLM geral: responde somente às classes para as quais há algoritmo ou evidência suficiente.

### 4. GRU próprio e aprendizado experimental

`neural_model.cpp` expõe matrizes, estados e gradientes. Para cada camada GRU, calcula explicitamente gates de atualização/reset, estado candidato, softmax e entropia cruzada. O treino usa BPTT truncado, clipping de gradiente e Adam, incluindo persistência dos dois momentos do otimizador.

A configuração padrão treinada pelo corpus incluído possui aproximadamente **1.003.428 parâmetros**. Durante as épocas, o melhor checkpoint é mantido em RAM e somente ele é publicado, evitando que uma oscilação tardia do Adam degrade o estado salvo ou cause várias escritas no HD. A rede é útil para experimentação, embedding e fallback restrito; não contém conhecimento comparável ao Granite.

O embedding episódico atual combina:

- 80% feature hashing lexical;
- 20% representação GRU;
- no máximo 96 tokens processados pelo GRU por consulta;
- normalização L2 antes da similaridade.

Isso dá mais peso aos termos reais, reduz falsos positivos de uma rede pouco treinada e mantém custo previsível no i5.

### 5. Estado e memória de longo prazo

`LongTermMemory` persiste id, timestamp, texto e vetor. A busca combina índice lexical, cobertura de termos e cosseno sobre embeddings, distribuindo a pontuação entre workers nativos. Os limiares padrão são:

| Modo | score mínimo |
|---|---:|
| Flash | 0,50 |
| Medium | 0,35 |
| High | 0,25 |

Memórias, documentos e resultados web não são confundidos com pesos. Isso permite aprender persistentemente sem retreinar o modelo inteiro e mantém a origem `[DOC]` ou `[WEB]` visível.

### 6. Qualidade e autoanálise

`quality.cpp` mede estrutura, diversidade lexical, repetição, legibilidade heurística, coerência estrutural e aderência à pergunta. O mesmo filtro protege respostas de GGUF e do GRU. Marcadores de papéis, loops, evasão e saudações genéricas desconectadas são rejeitados.

`BehaviorMonitor` registra somente contagens e médias agregadas: quantidade de respostas, rejeições, revisões e score. Esses agregados são persistidos em `state/behavior.stats` e restaurados na próxima sessão, sem copiar o conteúdo da conversa. O botão **Sugerir evolução automática** combina essa telemetria com auditoria estática para escolher uma fonte e gerar um patch; ainda assim, nada é aplicado sem `APLICAR N`. `analise sua resposta` explica sinais observáveis, mas não alega consciência ou acesso a uma “mente interna”. Métricas textuais também não provam veracidade factual.

### 7. Autoevolução supervisionada

`SafeSelfEditor` oferece o ciclo:

```text
auditar → propor → mostrar diff → APLICAR N → validar → manter ou REVERTER N
```

Para arquivos existentes, o Granite recebe somente trechos relevantes selecionados lexicalmente e responde com blocos exatos:

```text
<<<<<<< SEARCH
trecho atual único
=======
trecho novo
>>>>>>> REPLACE
```

Cortex verifica correspondência exata e não ambígua, recompõe o arquivo inteiro em memória, valida tamanho/NUL/balanceamento de fontes, confere o hash contra alterações concorrentes e somente então registra a proposta. Isso permite modificar fontes maiores que a janela de 4.096 tokens sem truncar regiões não relacionadas.

Garantias operacionais:

- nenhuma proposta escreve por si só;
- `APLICAR N`, `EXECUTAR N` e `REVERTER N` são confirmações diferentes e de uso único;
- escrita atômica por arquivo temporário + rename;
- backup e journal persistentes em `.cortex-backups/`;
- validação registra saída em `.cortex-validation/`;
- rollback falha se outra alteração mudou o alvo depois;
- o modo padrão restringe ao projeto;
- `--unrestricted-self-edit` aceita caminhos externos/criação e comandos livres, mas somente em loopback e com as mesmas confirmações.

## Árvore do projeto

```text
cortex-native/
├── include/cortex/
│   ├── agent.hpp               orquestrador e opções de recuperação
│   ├── connectivity.hpp        HTTP nativo/curl e protocolo de linguagem
│   ├── core.hpp                erros, matrizes, RNG e utilidades
│   ├── knowledge.hpp           API da base curada
│   ├── local_model.hpp         perfis GGUF e supervisor llama.cpp
│   ├── memory.hpp              memória e índice híbrido
│   ├── native_cognition.hpp    intents/resultados do Cognitive Mesh
│   ├── neural_model.hpp        GRU, gradientes e checkpoints
│   ├── quality.hpp             métricas, filtro e potência
│   ├── reasoner.hpp            ferramentas determinísticas
│   ├── self_editor.hpp         propostas, confirmação e rollback
│   ├── tokenizer.hpp           BPE byte-level
│   └── web_server.hpp          servidor HTTP e configuração
├── src/
│   ├── agent.cpp               pipeline nativo completo
│   ├── connectivity.cpp        transporte HTTPS/OpenAI-compatible
│   ├── core.cpp                álgebra e concorrência
│   ├── knowledge.cpp           70 temas multidisciplinares
│   ├── local_model.cpp         Granite/llama-server
│   ├── main.cpp                CLI, treino, ingestão e inicialização
│   ├── memory.cpp              ranking e persistência
│   ├── native_cognition.cpp    especialistas e síntese
│   ├── neural_model.cpp        forward/BPTT/Adam explícitos
│   ├── quality.cpp             análise universal de resposta
│   ├── reasoner.cpp            cálculo, benchmark e regras seguras
│   ├── self_editor.cpp         patches segmentados e transações
│   ├── tokenizer.cpp           treino/encode/decode BPE
│   ├── web_server.cpp          API HTTP, chat e segurança
│   └── web_ui.cpp              HTML/CSS/JS de navegador incorporados
├── data/corpus.pt.txt          corpus pequeno, auditável e licenciado no projeto
├── tests/test_main.cpp         suíte sem rede/modelo externo
├── ci/cortex-windows.yml.example modelo da validação Windows
├── Makefile / CMakeLists.txt   builds C++20
├── build-windows.cmd           build e testes MinGW-w64
├── start-windows.cmd           Granite X40
├── start-windows-native.cmd    somente Cognitive Mesh/GRU
├── start-windows-system.cmd    modo irrestrito com UAC
└── Cortex-One-Click-Windows.cmd instalação assistida
```

A validação ativa do repositório fica em `.github/workflows/cortex-windows.yml` e executa a suíte e `build-windows.cmd` em Windows Server 2022 com MinGW-w64.

## Compilar e testar

Linux/macOS com GCC ou Clang C++20:

```bash
cd cortex-native
make test
make release
```

Sanitizers:

```bash
make sanitize-test
```

Windows: consulte [`WINDOWS.md`](WINDOWS.md). O build usa MinGW-w64 e somente bibliotecas do sistema (`ws2_32` e `shell32`).

## Preparar o estado

Treino padrão do GRU auxiliar:

```bash
./build/cortex train \
  --corpus data/corpus.pt.txt \
  --state state
```

Treino curto para verificação:

```bash
./build/cortex train --corpus data/corpus.pt.txt --state state-test --epochs 1
```

O estado inclui tokenizador, checkpoint GRU e memória. Não versione `state/` nem arquivos GGUF.

## Adicionar conhecimento

Um documento:

```bash
./build/cortex ingest \
  --file manual.md \
  --source manual-interno \
  --chunk 2048 \
  --state state
```

Uma coleção inteira:

```bash
./build/cortex ingest \
  --directory meus-documentos \
  --source biblioteca-local \
  --max-files 512 \
  --chunk 2048 \
  --state state
```

Consultar e remover:

```bash
./build/cortex recall --query "como funciona o rollback?" --state state --limit 5
./build/cortex forget --id 17 --state state
```

A ingestão em massa tem limites de 8 MiB por arquivo, 128 MiB por execução e 10.000 arquivos no máximo. Ajuste `--max-files` de forma conservadora no HD.

## Executar

### Totalmente nativo, sem GGUF

```bash
./build/cortex serve --state state --host 127.0.0.1 --port 8080 \
  --power medium --enable-self-edit --source-root .
```

### Samsung X40 / Granite 4.2 3B

Instale `llama-server` e execute:

```bash
./build/cortex models
./build/cortex serve --state state --host 127.0.0.1 --port 8080 \
  --x40 --power medium --enable-self-edit --source-root .
```

`--billion` continua aceito apenas como alias legado de `--x40`.

Perfis disponíveis:

| Perfil | RAM recomendada | Modelo | Contexto | Execução padrão |
|---|---:|---|---:|---|
| `tiny` | 3 GiB | Granite 3.3 2B Q3_K_M | 2.048 | CPU, 2 threads |
| `edge` | 4 GiB | Granite 4.2 3B Q3_K_M | 3.072 | CPU, 4 threads |
| `x40` | 6 GiB | Granite 4.2 3B Q4_K_M | 4.096 | CPU, 4 threads |
| `daily` | 12 GiB | Granite 4.2 8B Q4_K_M | 8.192 | GPU se disponível |
| `workstation` | 32 GiB | Granite 4.2 30B Q3_K_M | 8.192 | GPU se disponível |
| `max` | 40 GiB | Granite 4.2 30B Q5_K_M | 16.384 | GPU se disponível |

Com 4 GiB, a seleção automática usa `edge`; com pelo menos 6 GiB, usa `x40`. O HD afeta principalmente download e inicialização. Se o X40 tiver menos RAM livre que o esperado, feche outros programas ou use `--local-profile edge`.

Modelo já baixado:

```bash
./build/cortex serve --state state \
  --local-model /caminho/modelo.gguf \
  --local-name "Meu Granite" \
  --local-context 4096 --local-threads 4 --local-gpu-layers 0
```

## CLI útil

```bash
./build/cortex chat --state state
./build/cortex ask --state state --power high --prompt "Planeje um diagnóstico causal"
./build/cortex analyze --text "texto a avaliar"
./build/cortex remember --state state --text "O codinome é Aurora."
./build/cortex benchmark --state state --steps 5000
./build/cortex inspect --state state
./build/cortex doctor --state state
```

No chat de terminal: `:lembrar TEXTO`, `:recordar CONSULTA` e `:sair`.

## Interface HTTP local

Endpoints principais:

- `GET /` — interface de conversa;
- `GET /health` — health check;
- `GET /api/status` — modelo, memória e telemetria agregada;
- `POST /api/chat` — chat, potência, RAG, pesquisa e filtro;
- `POST /api/analyze` — análise textual determinística;
- `POST /api/remember` — memória explícita;
- `POST /api/knowledge/ingest` — documento enviado pela interface;
- `POST /api/code/audit` — auditoria estática;
- `POST /api/code/refine` — auditoria + proposta de refinamento;
- `POST /api/code/evolve` — seleção automática por auditoria/telemetria + proposta;
- `POST /api/code/propose` — proposta direta;
- `POST /api/code/apply` — escrita após `APLICAR N`;
- `POST /api/code/rollback` — restauração após `REVERTER N`;
- `POST /api/validation/stage` — registra comando sem executar;
- `POST /api/validation/run` — execução única após `EXECUTAR N`.

O parser limita cabeçalhos e corpo, exige framing válido, usa CSP, não habilita CORS e protege rotas mutáveis por origem local. Não exponha o modo de autoedição diretamente à internet.

## Internet sem chave

A pesquisa usa DuckDuckGo e fallback da Wikipedia, registra URL HTTPS e pode consolidar o resultado como memória `[WEB]`. Um pedido explícito de pesquisa funciona sem API de linguagem. Use `--no-internet` para desativar toda pesquisa.

`curl` é usado como processo nativo quando o sistema não fornece uma biblioteca HTTP compilada; credenciais opcionais nunca são colocadas no argv nem em arquivo temporário. Integrações OpenAI-compatible continuam disponíveis por compatibilidade, mas não são necessárias nem ativadas automaticamente.

## Segurança e validação

Checkpoints e memórias validam magic/version, dimensões, quantidades, tamanhos e floats finitos antes de alocar. O servidor limita filas para backpressure. `Matrix` usa armazenamento `std::vector<float>` row-major; os pools matemático e HTTP são separados; o chamador participa do trabalho paralelo para reduzir espera.

A suíte atual executa **130 verificações** e cobre, entre outros pontos:

- BPE UTF-8 e persistência;
- forward, BPTT, Adam e redução de loss;
- matriz normal/transposta e determinismo;
- memória híbrida, documentos e ranking curado;
- benchmark real para a entrada `benchmark`;
- escrita longa, conversa, programação e raciocínio causal;
- perfis Granite e controles de thinking;
- JSON/HTTPS e proteção de segredos;
- filtro universal e monitor comportamental;
- patches SEARCH/REPLACE, arquivos grandes, hash, backup e rollback;
- criação irrestrita e validação autorizada.

## Limites e próximos passos técnicos

- A inferência Granite exige o download dos pesos e `llama-server`; sem eles, Cortex continua no Cognitive Mesh, porém com menor conhecimento geral.
- O contexto X40 é deliberadamente 4K para caber em RAM. Documentos e autoedição usam recuperação segmentada em vez de enviar tudo.
- A busca exata é adequada para coleções pequenas/médias. Milhões de trechos pedem HNSW/IVF e deduplicação persistente.
- O corpus incluído treina mecanismos, não um LLM moderno. Expanda conhecimento por documentos legais e pesos abertos, sem alegar cópia de conhecimento proprietário.
- Antes de distribuir para Windows, rode a suíte MinGW no hardware alvo, ASan/UBSan onde disponível e um teste real de inferência/rollback.
