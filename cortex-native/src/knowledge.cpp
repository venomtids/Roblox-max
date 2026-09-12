#include "cortex/knowledge.hpp"

#include "cortex/quality.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <vector>

namespace cortex {
namespace {

struct KnowledgeEntry final {
    std::string_view title;
    std::string_view category;
    std::string_view keywords;
    std::string_view text;
};

// Conteúdo deliberadamente curto, estável e revisável no código-fonte. Conhecimento
// volátil, médico individual ou jurídico deve vir de fonte atual e não desta tabela.
constexpr std::array<KnowledgeEntry, 70U> entries{{
    {"Programação e decomposição", "programação",
     "programacao programa software codigo decomposicao requisito problema",
     "Programar é transformar um problema em passos executáveis. Um processo confiável começa por "
     "definir entradas, saídas e invariantes, divide o trabalho em componentes pequenos, implementa "
     "uma versão verificável e mede o resultado com testes e observabilidade."},
    {"Algoritmos e complexidade", "programação",
     "algoritmo algoritmos complexidade big notacao desempenho tempo espaco",
     "Complexidade assintótica descreve como custo de tempo ou memória cresce com a entrada. O(1), "
     "O(log n), O(n), O(n log n) e O(n²) são classes de crescimento, não cronômetros: constantes, "
     "cache, alocação e distribuição dos dados também importam na medição real."},
    {"Estruturas de dados", "programação",
     "estrutura dados vetor lista pilha fila mapa hash arvore grafo heap",
     "Vetores oferecem boa localidade e acesso indexado; listas favorecem inserções locais, mas pagam "
     "com ponteiros e cache. Tabelas hash dão busca média próxima de O(1), árvores ordenadas mantêm "
     "ordem em O(log n), e grafos representam relações gerais. A escolha depende das operações dominantes."},
    {"C++ moderno e RAII", "programação",
     "cpp c++ raii destrutor recurso ponteiro smart unique shared memoria",
     "Em C++ moderno, RAII associa a vida de um recurso à vida de um objeto. `std::vector`, "
     "`std::string`, `std::unique_ptr` e locks com escopo reduzem vazamentos e caminhos de erro. "
     "`shared_ptr` só é adequado quando a propriedade realmente é compartilhada; referências e valores "
     "simples costumam expressar melhor as demais relações."},
    {"Comportamento indefinido em C++", "programação",
     "cpp c++ comportamento indefinido undefined behavior sanitizer overflow ponteiro lifetime",
     "Comportamento indefinido permite que o compilador assuma que certas situações nunca ocorrem. "
     "Acesso fora do vetor, uso após destruição, data race e overflow de inteiro com sinal são exemplos. "
     "Warnings, tipos com limites claros, ASan, UBSan e testes de fronteira ajudam a encontrá-los."},
    {"Ownership em Rust", "programação",
     "rust ownership propriedade borrow borrowing emprestimo lifetime cargo",
     "Rust representa propriedade e empréstimos no sistema de tipos. Um valor possui um dono, referências "
     "obedecem regras de aliasing e lifetimes impedem referências pendentes em muitos casos antes da "
     "execução. `unsafe` continua possível, mas isola obrigações que o compilador não consegue provar."},
    {"Concorrência correta", "programação",
     "concorrencia paralelismo thread mutex atomico race deadlock canal tarefa",
     "Concorrência organiza tarefas sobrepostas; paralelismo executa trabalho simultaneamente. Estado "
     "mutável compartilhado exige sincronização: mutex protege invariantes compostos, atômicos servem a "
     "operações pequenas e filas transferem propriedade. Ordem global de locks ajuda a evitar deadlocks."},
    {"Sistemas operacionais", "computação",
     "sistema operacional processo thread memoria virtual kernel syscall arquivo escalonamento",
     "O sistema operacional isola processos, agenda threads, oferece memória virtual, arquivos, rede e "
     "chamadas de sistema. Memória virtual mapeia endereços do processo para páginas físicas ou arquivos; "
     "isso melhora isolamento, mas page faults e troca para disco têm custo mensurável."},
    {"Compiladores", "computação",
     "compilador lexer parser ast otimizacao assembly link linker codigo maquina",
     "Um compilador normalmente tokeniza o fonte, constrói uma árvore sintática, verifica tipos, gera uma "
     "representação intermediária, otimiza e emite código. O linker resolve símbolos entre objetos e "
     "bibliotecas. Otimização preserva a semântica definida, não comportamentos indefinidos."},
    {"HTTP e APIs", "programação",
     "http https api rest metodo get post status json idempotencia endpoint",
     "HTTP troca requisições e respostas com método, alvo, cabeçalhos, status e corpo. GET deve ser seguro "
     "e normalmente idempotente; PUT é idempotente por definição, POST não necessariamente. Uma API robusta "
     "valida limites, usa códigos de status coerentes, versiona contratos e nunca confia no cliente."},
    {"TLS e segurança de transporte", "segurança",
     "tls https certificado criptografia transporte chave handshake autenticidade",
     "TLS autentica o servidor por uma cadeia de certificados e negocia chaves de sessão para confidencialidade "
     "e integridade. HTTPS não torna o conteúdo verdadeiro nem corrige autorização ruim; ele protege o canal. "
     "Ignorar validação de certificado remove uma garantia central do protocolo."},
    {"TCP, UDP e DNS", "redes",
     "tcp udp dns rede pacote latencia conexao retransmissao porta dominio",
     "TCP fornece um fluxo ordenado e retransmite perdas; UDP envia datagramas sem essas garantias e permite "
     "que a aplicação escolha sua estratégia. DNS traduz nomes para registros como endereços IP. Latência, "
     "largura de banda, perda e congestionamento são propriedades diferentes da rede."},
    {"Bancos relacionais e transações", "programação",
     "sql banco dados relacional transacao acid commit rollback isolamento tabela",
     "Uma transação agrupa operações com propriedades de atomicidade, consistência, isolamento e durabilidade. "
     "Chaves e constraints preservam invariantes no banco. O nível de isolamento define quais interações "
     "concorrentes podem aparecer; transações longas aumentam contenção e retenção de versões."},
    {"Índices de banco de dados", "programação",
     "sql indice index btree consulta query banco desempenho explain",
     "Índices aceleram filtros e ordenações ao manter uma estrutura auxiliar, frequentemente uma B-tree. "
     "Eles consomem espaço e tornam escritas mais caras. A ordem das colunas, seletividade e plano mostrado "
     "por EXPLAIN devem orientar a escolha, em vez de criar um índice para toda coluna."},
    {"Git e histórico", "programação",
     "git commit branch merge rebase conflito versionamento repositorio",
     "Git armazena snapshots ligados por commits. Branches são referências móveis; merge preserva as duas "
     "linhas de história e rebase reaplica commits sobre outra base. Commits pequenos, mensagens causais e "
     "testes antes da integração tornam revisão e rollback mais seguros."},
    {"Testes de software", "programação",
     "teste testes unitario integracao sistema propriedade regressao cobertura mock",
     "Testes unitários isolam regras pequenas, testes de integração verificam fronteiras reais e testes de "
     "sistema exercitam o produto. Cobertura mostra código executado, não correção. Um bom teste controla "
     "fontes de aleatoriedade, verifica comportamento observável e falha por uma causa compreensível."},
    {"Debug e profiling", "programação",
     "debug depuracao profiling profiler desempenho benchmark log tracing",
     "Depuração parte de uma reprodução mínima e de uma hipótese falsificável. Logs estruturados e tracing "
     "expõem causalidade; sanitizers encontram classes de erro. Para desempenho, meça antes de otimizar, use "
     "um profiler e compare distribuições de latência, não apenas uma média isolada."},
    {"Arquitetura de software", "programação",
     "arquitetura modulo acoplamento coesao interface dependencia solid componente",
     "Módulos devem esconder decisões internas atrás de contratos pequenos. Alta coesão mantém responsabilidades "
     "relacionadas juntas; baixo acoplamento reduz efeitos em cascata. Camadas só ajudam quando representam "
     "fronteiras reais, e abstrações prematuras podem ser mais caras que duplicação temporária."},
    {"Programação funcional", "programação",
     "funcional funcao pura imutabilidade efeito map reduce recursao",
     "Funções puras devolvem o mesmo resultado para a mesma entrada e não alteram estado externo observável. "
     "Imutabilidade facilita composição e concorrência, enquanto efeitos continuam necessários nas bordas. "
     "O objetivo prático é tornar transformação de dados previsível, não proibir todo estado."},
    {"Recursão e iteração", "programação",
     "recursao iteracao pilha caso base algoritmo fatorial",
     "Recursão resolve uma instância usando instâncias menores e exige um caso-base. Ela expressa árvores e "
     "divisão e conquista com clareza, mas cada chamada pode consumir pilha. Uma forma iterativa ou uma pilha "
     "explícita é preferível quando a profundidade pode crescer sem limite seguro."},
    {"Unicode e UTF-8", "programação",
     "unicode utf utf8 caractere byte codepoint grapheme texto encoding",
     "UTF-8 codifica code points Unicode em um a quatro bytes. Bytes, code points e grafemas visíveis não são "
     "a mesma unidade: um caractere percebido pode combinar vários code points. Cortes, índices e limites devem "
     "declarar qual unidade usam e nunca separar uma sequência UTF-8 no meio."},
    {"Segurança de entrada", "segurança",
     "seguranca validacao entrada injecao path traversal ssrf limite autenticacao autorizacao",
     "Entradas externas precisam de limites de tamanho, gramática e autorização. Parametrização evita injeção "
     "SQL; canonicalização e allowlists reduzem path traversal; destinos fixos e HTTPS reduzem SSRF. "
     "Autenticação identifica uma entidade, enquanto autorização decide o que ela pode fazer."},
    {"Transformers e atenção", "inteligência artificial",
     "transformer atencao attention token camada modelo linguagem llm",
     "Transformers calculam representações de tokens usando atenção, que pondera relações com outros tokens, "
     "e redes feed-forward por camada. O custo de atenção padrão cresce quadraticamente com o contexto. "
     "Parâmetros armazenam padrões aprendidos, mas não garantem fatos corretos nem raciocínio infalível."},
    {"RAG e memória externa", "inteligência artificial",
     "rag retrieval recuperacao memoria embedding vetor contexto documento",
     "RAG recupera documentos antes da geração e os coloca no contexto. Embeddings ajudam a encontrar "
     "proximidade semântica, mas podem recuperar material irrelevante; filtros lexicais, metadados, fontes e "
     "avaliação do resultado são necessários. RAG atualiza conhecimento sem retreinar todos os pesos."},
    {"Quantização de modelos", "inteligência artificial",
     "quantizacao gguf q4 q8 pesos modelo memoria inferencia llama",
     "Quantização representa pesos com menos bits para reduzir RAM e largura de banda. Q8 tende a preservar "
     "mais fidelidade e Q4 economiza mais, com perda dependente do modelo e da tarefa. GGUF empacota pesos e "
     "metadados para runtimes como llama.cpp; quantização não muda a contagem nominal de parâmetros."},
    {"Avaliação de IA", "inteligência artificial",
     "avaliacao inteligencia artificial benchmark alucinacao qualidade dataset teste modelo",
     "Qualidade de IA deve ser medida em tarefas separadas do treino, com critérios reproduzíveis. Acurácia, "
     "latência, memória, robustez e taxa de afirmações sem suporte capturam riscos diferentes. Uma demonstração "
     "bem escolhida não prova generalização; casos adversariais e regressões também precisam entrar no conjunto."},
    {"Probabilidade e estatística", "matemática",
     "probabilidade estatistica media mediana variancia correlacao causalidade amostra",
     "Média resume soma por contagem, mediana resiste melhor a extremos e variância mede dispersão quadrática. "
     "Correlação descreve associação, não prova causalidade. Amostragem enviesada, confundidores e múltiplas "
     "comparações podem produzir conclusões convincentes e erradas."},
    {"Método científico", "ciência",
     "ciencia metodo cientifico hipotese experimento evidencia reproducao teoria",
     "O método científico formula hipóteses testáveis, deriva previsões, coleta evidência e revisa explicações. "
     "Reprodutibilidade, controles, transparência e revisão crítica reduzem erros. Uma teoria científica é uma "
     "estrutura explicativa sustentada por evidências, não um simples palpite."},
    {"Células e evolução", "biologia",
     "biologia celula dna gene evolucao selecao natural organismo",
     "Células mantêm metabolismo, informação genética e fronteiras reguladas. DNA armazena sequências herdáveis, "
     "mas ambiente e regulação influenciam características. Evolução ocorre quando variação herdável e diferenças "
     "de reprodução mudam frequências em populações ao longo de gerações."},
    {"Energia e potência", "física",
     "fisica energia potencia trabalho joule watt conservacao",
     "Energia é uma grandeza conservada em sistemas isolados e pode mudar de forma. Trabalho transfere energia "
     "por uma força ao longo de um deslocamento; potência mede a taxa dessa transferência. Joule mede energia e "
     "watt equivale a joule por segundo."},
    {"Clima e tempo", "ciência",
     "clima tempo atmosfera aquecimento efeito estufa carbono meteorologia",
     "Tempo descreve condições atmosféricas de curto prazo; clima descreve distribuições e tendências em períodos "
     "longos. Gases de efeito estufa absorvem e reemitem radiação infravermelha. Um evento isolado não determina "
     "uma tendência climática, que exige séries, modelos e análise de incerteza."},
    {"Inflação e poder de compra", "economia",
     "economia inflacao preco moeda juros poder compra indice",
     "Inflação é aumento persistente do nível geral de preços, medido por uma cesta e um índice definidos. Ela "
     "reduz o poder de compra de uma unidade monetária, mas afeta grupos de modo diferente. Oferta, demanda, "
     "expectativas, câmbio e política monetária podem interagir; uma única causa raramente explica todo episódio."},
    {"Leitura crítica de história", "humanidades",
     "historia fonte documento evidencia contexto vies historiografia",
     "Análise histórica compara fontes, autoria, data, propósito, público e contexto. Fontes primárias são próximas "
     "ao evento, mas não automaticamente imparciais; trabalhos secundários interpretam conjuntos de evidências. "
     "Convergência independente e explicações concorrentes aumentam a confiabilidade."},
    {"Escrita de textos complexos", "comunicação",
     "escrita texto complexo redacao argumento paragrafo revisao clareza",
     "Um texto complexo continua precisando de uma tese clara. Organize seções por função, abra parágrafos com "
     "uma ideia controladora, conecte evidências à conclusão e trate objeções relevantes. Na revisão, remova "
     "redundância, verifique termos ambíguos e separe afirmações factuais de inferências."},
    {"Git e histórico", "programação",
     "git commit branch ramo merge rebase versionamento historico conflito",
     "Git registra snapshots em commits ligados por um grafo. Branches são referências móveis; merge preserva histórias, "
     "enquanto rebase reaplica commits e altera seus identificadores. Mudanças compartilhadas devem ser revisadas e testadas."},
    {"HTTP e métodos", "redes",
     "http https get post put patch delete metodo status cabecalho",
     "HTTP é um protocolo de requisição e resposta. GET deve ser seguro, PUT é definido como idempotente e POST não é "
     "necessariamente idempotente. Status, cabeçalhos, cache e corpo fazem parte do contrato; HTTPS adiciona TLS ao canal."},
    {"DNS", "redes",
     "dns dominio nome endereco ip resolver cache ttl",
     "DNS traduz nomes em registros distribuídos, como A, AAAA, MX e CNAME. Resolvers usam cache limitado por TTL; "
     "propagação não é instantânea e DNS tradicional, sozinho, não oferece confidencialidade."},
    {"TCP e UDP", "redes",
     "tcp udp rede pacote transporte conexao latencia confiabilidade",
     "TCP oferece fluxo ordenado, retransmissão e controle de congestionamento. UDP preserva datagramas e reduz estado, "
     "mas a aplicação decide como tratar perda, ordem e retransmissão. A escolha depende do contrato, não só de velocidade."},
    {"Cache", "sistemas",
     "cache localidade hit miss ttl invalidacao desempenho memoria",
     "Cache troca espaço por menor latência ao explorar localidade. Políticas de expiração e invalidação determinam "
     "correção; medir hit rate, custo de miss e pressão de memória evita otimizar apenas o caso feliz."},
    {"Consenso distribuído", "sistemas distribuídos",
     "consenso raft paxos distribuido quorum lider replicacao falha",
     "Protocolos de consenso permitem que réplicas concordem sobre uma ordem apesar de certas falhas. Quórum, eleição "
     "de líder e log replicado não eliminam partições; disponibilidade e consistência dependem do modelo adotado."},
    {"Contêineres e máquinas virtuais", "sistemas",
     "container conteiner maquina virtual isolamento namespace hipervisor imagem",
     "Máquinas virtuais virtualizam hardware e carregam kernels separados. Contêineres normalmente isolam processos que "
     "compartilham o kernel do host; imagens reproduzíveis ajudam implantação, mas não substituem controles de segurança."},
    {"C# e gerenciamento de memória", "programação",
     "csharp c# dotnet clr garbage collector async await memoria",
     "C# é tipado e normalmente executado pela CLR. O coletor recupera objetos gerenciados inalcançáveis, mas recursos "
     "externos ainda pedem IDisposable/using. async/await coordena continuations e não cria paralelismo automaticamente."},
    {"Java e JVM", "programação",
     "java jvm bytecode garbage collector interface memoria",
     "Java compila normalmente para bytecode executado pela JVM. Tipos, interfaces, JIT e coleta geracional apoiam "
     "portabilidade e desempenho; medir alocação e pausas continua necessário em serviços sensíveis à latência."},
    {"Go e goroutines", "programação",
     "golang go goroutine channel canal concorrencia contexto",
     "Goroutines são tarefas leves multiplexadas pelo runtime Go. Channels coordenam comunicação, mas estado compartilhado "
     "ainda pode sofrer data race; contextos propagam cancelamento e prazo entre operações cooperativas."},
    {"Zig", "programação",
     "zig linguagem compilador allocator comptime memoria erro",
     "Zig é uma linguagem compilada com alocadores explícitos, erros como valores e avaliação comptime. Ela busca controle "
     "previsível e interoperabilidade C, mas ownership e lifetime ainda são responsabilidade do projeto."},
    {"Programação funcional", "programação",
     "funcional funcao pura imutabilidade recursao efeito composicao",
     "Funções puras e dados imutáveis reduzem estados implícitos e facilitam composição e testes. Efeitos ainda existem; "
     "uma arquitetura funcional os torna explícitos nas fronteiras em vez de fingir que entrada e saída desapareceram."},
    {"Compiladores", "computação",
     "compilador lexer parser ast ir otimizacao codigo maquina",
     "Um compilador costuma tokenizar, construir uma árvore sintática, verificar tipos, produzir representação intermediária, "
     "otimizar e emitir código. Cada etapa preserva invariantes e deve produzir diagnósticos úteis para entradas inválidas."},
    {"Unicode e UTF-8", "computação",
     "unicode utf8 codepoint caractere byte normalizacao texto",
     "UTF-8 codifica code points Unicode em um a quatro bytes e preserva ASCII. Bytes, code points e grafemas não são a "
     "mesma unidade; truncar por byte ou comparar sem uma política de normalização pode corromper ou confundir texto."},
    {"Hashes criptográficos", "segurança",
     "hash criptografico sha256 integridade colisao senha digest",
     "Um hash criptográfico mapeia dados para um digest e deve resistir a preimagem e colisões. Hash não cifra dados. "
     "Senhas exigem derivação lenta com salt, como Argon2, em vez de um hash geral rápido."},
    {"Criptografia de chave pública", "segurança",
     "criptografia chave publica privada assinatura certificado rsa curva",
     "Criptografia assimétrica usa pares de chaves para troca de segredo ou assinatura. Assinaturas dão integridade e "
     "autenticidade, não sigilo; certificados ligam identidades a chaves dentro de uma cadeia de confiança."},
    {"Teorema de Bayes", "estatística",
     "bayes probabilidade condicional prior posterior evidencia",
     "Bayes atualiza uma probabilidade anterior pela verossimilhança da evidência: P(A|B)=P(B|A)P(A)/P(B). "
     "O resultado depende do modelo, dos priors e da qualidade da evidência, não apenas da fórmula."},
    {"Álgebra linear", "matemática",
     "algebra linear vetor matriz transformacao autovalor sistema",
     "Vetores representam elementos de um espaço e matrizes representam transformações lineares em uma base. Produto, "
     "posto, condicionamento e decomposições ajudam a resolver sistemas e entender estabilidade numérica."},
    {"Derivada e integral", "matemática",
     "calculo derivada integral limite taxa area gradiente",
     "A derivada descreve taxa de variação local; a integral acumula quantidades. O teorema fundamental do cálculo conecta "
     "as duas sob condições adequadas. Métodos numéricos precisam controlar discretização e erro de arredondamento."},
    {"Lógica e argumentos", "filosofia",
     "logica argumento premissa conclusao validade falacia deducao",
     "Validade significa que a conclusão segue das premissas; não garante que elas sejam verdadeiras. Um argumento sólido "
     "precisa de forma válida e premissas justificadas, e uma falácia identifica um defeito de inferência relevante."},
    {"Sistema Solar", "astronomia",
     "astronomia sistema solar planeta sol terra lua orbita gravidade",
     "O Sistema Solar contém o Sol, oito planetas reconhecidos e muitos corpos menores ligados pela gravidade. A Terra "
     "orbita o Sol e a Lua orbita a Terra; escalas astronômicas exigem distinguir modelos aproximados de medidas atuais."},
    {"Átomos e moléculas", "química",
     "quimica atomo molecula elemento proton eletron ligacao reacao",
     "Átomos têm núcleo com prótons e nêutrons e uma distribuição eletrônica. Elementos são definidos pelo número de "
     "prótons; ligações formam moléculas, e reações rearranjam átomos sem criar carga ou massa do nada no modelo usual."},
    {"Tempo e clima", "ciência da Terra",
     "clima tempo atmosfera temperatura previsao aquecimento estufa",
     "Tempo descreve condições atmosféricas de curto prazo; clima descreve distribuições ao longo de décadas. Um evento "
     "isolado não determina uma tendência, enquanto gases de efeito estufa alteram o balanço energético do planeta."},
    {"Evolução biológica", "biologia",
     "biologia evolucao selecao natural gene mutacao populacao especie",
     "Evolução é mudança herdável em populações ao longo de gerações. Mutação, seleção, deriva e fluxo gênico contribuem; "
     "indivíduos não evoluem por necessidade e ancestralidade comum não afirma uma marcha linear de progresso."},
    {"Evidência em saúde", "saúde",
     "saude medicina sintoma diagnostico tratamento evidencia ensaio risco",
     "Informação geral não substitui avaliação clínica. Diagnóstico combina história, exame e testes com benefícios e "
     "riscos; sinais graves ou urgentes exigem serviço de saúde, e tratamentos devem apoiar-se em evidência aplicável."},
    {"Geografia do Brasil", "geografia",
     "brasil geografia brasilia capital regioes estados america sul",
     "Brasil fica na América do Sul, possui 26 estados e o Distrito Federal e tem Brasília como capital desde 1960. "
     "As cinco grandes regiões do IBGE são Norte, Nordeste, Centro-Oeste, Sudeste e Sul."},
    {"Revolução Industrial", "história",
     "historia revolucao industrial fabrica vapor urbanizacao trabalho",
     "A Revolução Industrial começou na Grã-Bretanha no século XVIII e difundiu produção mecanizada, fábricas e novas "
     "fontes de energia. Ela elevou produtividade e urbanização, ao mesmo tempo que transformou trabalho e desigualdade."},
    {"Separação de poderes", "cidadania",
     "governo poderes executivo legislativo judiciario constituicao democracia",
     "A separação entre Executivo, Legislativo e Judiciário distribui competências e cria controles recíprocos. O desenho "
     "concreto varia por constituição; independência institucional não significa ausência de responsabilidade pública."},
    {"Informação jurídica", "direito",
     "direito lei contrato processo jurisdicao advogado juridico",
     "Leis e procedimentos variam por país, data e jurisdição. Uma explicação geral deve identificar essas condições e "
     "não substituir aconselhamento profissional quando direitos, prazos, patrimônio ou liberdade estiverem em risco."},
    {"Juros compostos", "economia",
     "juros compostos investimento divida principal taxa periodo inflacao",
     "Em juros compostos, cada período incide sobre principal e juros acumulados: M=P(1+i)^n para taxa e períodos "
     "compatíveis. Custos, impostos, inflação e risco separam retorno nominal de ganho real."},
    {"Acessibilidade web", "engenharia de software",
     "acessibilidade web wcag teclado contraste aria semantica leitor tela",
     "HTML semântico, foco visível, operação por teclado, contraste e rótulos associados formam a base da acessibilidade. "
     "ARIA complementa a semântica quando necessário e testes com tecnologia assistiva revelam problemas reais."},
    {"Visualização de dados", "estatística",
     "grafico visualizacao dados eixo escala cor comparacao distribuicao",
     "Uma visualização deve corresponder à pergunta: barras comparam categorias, linhas mostram sequência e histogramas "
     "mostram distribuição. Escalas truncadas, áreas enganosas e cores sem legenda podem distorcer a interpretação."},
    {"Avaliação de modelos", "IA",
     "modelo avaliacao benchmark precision recall f1 acuracia validacao drift",
     "Métricas dependem do custo dos erros: precisão mede acerto entre positivos previstos, recall mede positivos encontrados "
     "e F1 equilibra ambos. Validação separada, análise por subgrupo e monitoramento de drift evitam um único número enganoso."},
    {"Requisitos e evolução", "engenharia de software",
     "requisito aceitacao escopo iteracao entrega risco projeto",
     "Requisitos úteis descrevem resultado observável e critérios de aceitação. Entregas pequenas reduzem risco, mas cada "
     "incremento precisa manter compatibilidade, testes e um caminho claro de rollback."},
    {"Autoconhecimento da Cortex", "Cortex",
     "cortex identidade autoconhecimento autoentendimento comportamento arquitetura limite",
     "Cortex é um sistema híbrido: ferramentas determinísticas, base curada, pesquisa com fontes, memória vetorial, "
     "um GRU auxiliar e um gerador GGUF opcional. Ela pode medir estrutura e relevância do próprio texto, mas essa "
     "autoanálise é um algoritmo, não consciência, intenção ou experiência subjetiva."},
    {"Autoevolução controlada", "Cortex",
     "autoevolucao autoprogramacao autoedicao refinar codigo auditoria proposta rollback",
     "A autoevolução é um ciclo de observar, auditar, propor, autorizar, validar e publicar. No modo irrestrito "
     "supervisionado, Cortex pode criar ou substituir qualquer arquivo aceito pela conta do sistema e executar um "
     "comando mostrado previamente; cada escrita/execução exige permissão de uso único e mantém rollback quando aplicável."}
}};

[[nodiscard]] const std::vector<std::vector<std::string>>& indexed_terms() {
    // Inicialização local é thread-safe desde C++11. O índice lexical é construído
    // uma única vez, em vez de tokenizar todas as 70 entradas a cada requisição.
    static const std::vector<std::vector<std::string>> index = [] {
        std::vector<std::vector<std::string>> value;
        value.reserve(entries.size());
        for (const KnowledgeEntry& entry : entries) {
            value.push_back(significant_words(
                std::string(entry.title) + " " + std::string(entry.keywords)));
        }
        return value;
    }();
    return index;
}

} // namespace

std::vector<CuratedKnowledgeAnswer> search_curated_knowledge(
    std::string_view query,
    std::size_t limit,
    double minimum_score) {
    if (limit == 0U || minimum_score < 0.0 || minimum_score > 1.0) return {};
    std::vector<std::string> query_words = significant_words(query);
    if (query_words.empty()) return {};

    // Verbos de comando descrevem o formato desejado, não o assunto. Removê-los evita
    // que "explique" dilua a pontuação de uma pergunta curta.
    constexpr std::array<std::string_view, 25U> command_words{
        "analise", "artigo", "compare", "conte", "crie", "defina", "demonstre",
        "descreva", "detalhado", "detalhe", "diferenca", "ensaio", "escreva", "explique",
        "fale", "funciona", "longo", "longa", "mostre", "porque", "redija", "resuma",
        "sobre", "texto", "tutorial"
    };
    query_words.erase(std::remove_if(query_words.begin(), query_words.end(), [&](const std::string& word) {
        return std::find(command_words.begin(), command_words.end(), word) != command_words.end();
    }), query_words.end());
    if (query_words.empty()) return {};

    struct RankedEntry final {
        std::size_t index = 0U;
        std::size_t exact_overlap = 0U;
        double score = 0.0;
    };
    std::vector<RankedEntry> ranked;
    const auto& index = indexed_terms();
    ranked.reserve(entries.size());

    const auto related = [](std::string_view left, std::string_view right) noexcept {
        if (left == right) return 1.0;
        const std::size_t common_limit = std::min(left.size(), right.size());
        std::size_t common = 0U;
        while (common < common_limit && left[common] == right[common]) ++common;
        // Um radical longo cobre flexões simples em português sem um stemmer opaco.
        return common >= 5U && common + 3U >= common_limit ? 0.68 : 0.0;
    };

    for (std::size_t entry_index = 0U; entry_index < entries.size(); ++entry_index) {
        const std::vector<std::string>& terms = index[entry_index];
        double weighted_overlap = 0.0;
        std::size_t exact_overlap = 0U;
        bool has_discriminative_exact_term = false;
        constexpr std::array<std::string_view, 11U> broad_terms{
            "codigo", "dados", "estrutura", "estruturas", "modelo", "modelos",
            "programacao", "rede", "redes", "sistema", "sistemas"
        };
        for (const std::string& word : query_words) {
            double best_term = 0.0;
            for (const std::string& term : terms) {
                best_term = std::max(best_term, related(word, term));
                if (best_term == 1.0) break;
            }
            weighted_overlap += best_term;
            if (best_term == 1.0) {
                ++exact_overlap;
                if (std::find(broad_terms.begin(), broad_terms.end(), word) == broad_terms.end()) {
                    has_discriminative_exact_term = true;
                }
            }
        }
        if (weighted_overlap <= 0.0) continue;
        const double query_coverage = weighted_overlap
                                      / static_cast<double>(query_words.size());
        const double topic_precision = weighted_overlap
                                       / static_cast<double>(std::max<std::size_t>(terms.size(), 1U));
        const double exact_bonus = static_cast<double>(exact_overlap)
                                   / static_cast<double>(query_words.size()) * 0.12;
        const double score = std::min(1.0, query_coverage * 0.78
                                      + topic_precision * 0.10 + exact_bonus);
        // Consultas longas com apenas uma coincidência fraca continuam sem resposta factual.
        if (score >= minimum_score
            && (query_words.size() < 3U || exact_overlap >= 2U
                || (query_words.size() == 3U && has_discriminative_exact_term)
                || score >= 0.48)) {
            ranked.push_back(RankedEntry{entry_index, exact_overlap, score});
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const RankedEntry& left, const RankedEntry& right) {
        if (left.exact_overlap != right.exact_overlap) {
            return left.exact_overlap > right.exact_overlap;
        }
        if (left.score != right.score) return left.score > right.score;
        return left.index < right.index;
    });
    if (ranked.size() > limit) ranked.resize(limit);

    std::vector<CuratedKnowledgeAnswer> answers;
    answers.reserve(ranked.size());
    for (const RankedEntry& item : ranked) {
        const KnowledgeEntry& entry = entries[item.index];
        answers.push_back(CuratedKnowledgeAnswer{
            std::string(entry.title), std::string(entry.category), std::string(entry.text), item.score});
    }
    return answers;
}

std::optional<CuratedKnowledgeAnswer> lookup_curated_knowledge(std::string_view query) {
    std::vector<CuratedKnowledgeAnswer> answers = search_curated_knowledge(query, 1U);
    if (answers.empty()) return std::nullopt;
    CuratedKnowledgeAnswer answer = std::move(answers.front());
    answer.text = answer.title + "\n\n" + answer.text
        + "\n\nBase: conhecimento curado e estável da Cortex; para dados atuais, peça uma pesquisa.";
    return answer;
}

std::size_t curated_knowledge_size() noexcept {
    return entries.size();
}

} // namespace cortex
