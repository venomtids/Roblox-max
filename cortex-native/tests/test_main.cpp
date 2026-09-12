#include "cortex/connectivity.hpp"
#include "cortex/core.hpp"
#include "cortex/knowledge.hpp"
#include "cortex/local_model.hpp"
#include "cortex/memory.hpp"
#include "cortex/native_cognition.hpp"
#include "cortex/neural_model.hpp"
#include "cortex/quality.hpp"
#include "cortex/reasoner.hpp"
#include "cortex/self_editor.hpp"
#include "cortex/tokenizer.hpp"
#include "cortex/web_server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
std::size_t passed = 0U;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw cortex::Error("teste falhou: " + message);
    }
    ++passed;
}

[[nodiscard]] bool near(float left, float right, float tolerance = 1.0e-5F) {
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] fs::path temporary_test_directory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path directory = fs::temp_directory_path()
                         / ("cortex-native-tests-" + std::to_string(stamp));
    fs::create_directories(directory);
    return directory;
}

void test_connectivity_protocol() {
    expect(cortex::url_encode("ação e C++") == "a%C3%A7%C3%A3o%20e%20C%2B%2B",
           "URL encoder preserva UTF-8 por percent-encoding");
    const std::string response = R"({"choices":[{"message":{"content":"Olá\nMundo \ud83d\ude80"}}]})";
    expect(cortex::json_string_field(response, "content").value_or("")
               == "Olá\nMundo 🚀",
           "parser extrai conteúdo e surrogate pair da API linguística");
    cortex::NativeHttpClient::validate_url("https://api.example.com/v1/chat/completions");
    cortex::NativeHttpClient::validate_url("http://127.0.0.1:8081/v1/chat/completions");
    bool rejected = false;
    try {
        cortex::NativeHttpClient::validate_url("http://example.com/inseguro");
    } catch (const cortex::Error&) {
        rejected = true;
    }
    expect(rejected, "cliente recusa HTTP remoto sem TLS");

    cortex::ModelConfig million_config;
    million_config.vocabulary_size = 420U; // 4 especiais + 256 bytes + 160 merges.
    cortex::NeuralLanguageModel million_model(million_config);
    expect(million_model.parameter_count() == 1'003'428U,
           "configuração padrão possui 1.003.428 parâmetros treináveis");
}

void test_quality_knowledge_and_power_modes() {
    expect(cortex::parse_power_mode("rápido") == cortex::PowerMode::flash
               && cortex::parse_power_mode("medium") == cortex::PowerMode::medium
               && cortex::parse_power_mode("alta potência") == cortex::PowerMode::high,
           "modos Flash, Medium e Alta potência aceitam nomes em português");
    const cortex::PowerProfile& flash = cortex::power_profile(cortex::PowerMode::flash);
    const cortex::PowerProfile& high = cortex::power_profile(cortex::PowerMode::high);
    expect(flash.maximum_tokens < high.maximum_tokens && flash.memory_hits < high.memory_hits
               && !flash.revision_pass && high.revision_pass,
           "perfis mudam custo, contexto e segunda passagem de forma verificável");

    const std::string sample = "Primeiro parágrafo explica o objetivo.\n\n"
                               "Segundo parágrafo contém detalhes e revisão.";
    const cortex::TextMetrics metrics = cortex::analyze_text(sample);
    expect(metrics.paragraphs == 2U && metrics.sentences == 2U
               && metrics.words >= 10U && metrics.lexical_diversity > 0.5
               && metrics.readability_score > 0.7
               && metrics.structural_coherence_score > 0.7
               && !metrics.top_terms.empty(),
           "analisador mede estrutura, legibilidade, coerência, repetição e tópicos");
    const cortex::ResponseAssessment accepted = cortex::assess_response(
        "Explique RAII em C++",
        "RAII em C++ associa cada recurso ao lifetime de um objeto e ao seu destrutor.");
    expect(accepted.accepted && accepted.relevance_score > 0.5,
           "filtro aprova resposta coerente ligada à pergunta");
    const cortex::ResponseAssessment rejected = cortex::assess_response(
        "Explique RAII em C++", "Olá! Como posso ajudar com outro assunto?");
    expect(!rejected.accepted && !rejected.issues.empty(),
           "filtro rejeita evasão temática antes de exibi-la");
    expect(cortex::assess_response("Traduza hello", "Olá").accepted,
           "filtro não rejeita transformação curta apenas por mudar o vocabulário");

    cortex::BehaviorMonitor monitor;
    monitor.record(accepted, true);
    monitor.record(rejected, false);
    const cortex::BehaviorSnapshot behavior = monitor.snapshot();
    expect(behavior.response_count == 2U && behavior.rejected_count == 1U
               && behavior.revised_count == 1U && behavior.average_quality > 0.0,
           "monitor agrega qualidade e revisões sem armazenar o texto do usuário");
    cortex::BehaviorMonitor restored_monitor;
    restored_monitor.restore(behavior);
    const cortex::BehaviorSnapshot restored_behavior = restored_monitor.snapshot();
    expect(restored_behavior.response_count == behavior.response_count
               && restored_behavior.rejected_count == behavior.rejected_count
               && restored_behavior.revised_count == behavior.revised_count
               && std::abs(restored_behavior.average_quality - behavior.average_quality) < 0.000001,
           "telemetria agregada pode ser restaurada entre sessões sem guardar conversas");

    const auto programming = cortex::lookup_curated_knowledge(
        "Como RAII funciona em C++ moderno?");
    const auto general = cortex::lookup_curated_knowledge(
        "Explique inflação e poder de compra");
    const auto geography = cortex::lookup_curated_knowledge(
        "Quantos estados existem e qual é a capital do Brasil?");
    const auto networking = cortex::lookup_curated_knowledge(
        "Qual é a diferença entre TCP e UDP?");
    expect(programming && programming->category == "programação"
               && programming->text.find("std::unique_ptr") != std::string::npos,
           "base curada responde conhecimento de programação específico");
    expect(general && general->category == "economia"
               && general->text.find("poder de compra") != std::string::npos,
           "base curada também cobre conhecimento geral multidisciplinar");
    expect(geography && geography->category == "geografia"
               && geography->text.find("26 estados") != std::string::npos
               && networking && networking->category == "redes",
           "geografia e redes são recuperadas por intenção auditável");
    expect(cortex::curated_knowledge_size() == 70U,
           "base local possui setenta temas multidisciplinares auditáveis");
}

void test_native_cognitive_mesh() {
    const std::vector<cortex::CuratedKnowledgeAnswer> evidence =
        cortex::search_curated_knowledge("compare concorrência, threads, mutex e estruturas de dados", 3U);
    expect(evidence.size() >= 2U && evidence.front().score > 0.0
               && std::none_of(evidence.begin(), evidence.end(), [](const auto& item) {
                      return item.title == "Visualização de dados"
                             || item.title == "Índices de banco de dados";
                  }),
           "busca curada múltipla prioriza evidência específica sem deriva por termo genérico");

    cortex::NativeCognitionOptions medium;
    medium.power_mode = cortex::PowerMode::medium;
    const auto grounded = cortex::synthesize_native_response(
        "Compare concorrência e estruturas de dados", {}, {}, medium, evidence);
    expect(grounded && grounded->intent == cortex::NativeCognitiveIntent::comparison
               && grounded->evidence_count >= 2U
               && grounded->text.find("Critério") != std::string::npos,
           "Cognitive Mesh cruza múltiplas evidências em uma comparação estruturada");

    const auto creative = cortex::synthesize_native_response(
        "Escreva uma história criativa sobre robôs aprendendo com erros", {}, {}, medium);
    expect(creative && creative->intent == cortex::NativeCognitiveIntent::creative_writing
               && creative->text.find("robôs aprendendo com erros") != std::string::npos
               && creative->text.size() > 500U,
           "especialista nativo produz narrativa longa com arco e tema solicitado");

    const auto article = cortex::synthesize_native_response(
        "Redija um artigo longo sobre concorrência e estruturas de dados", {}, {}, medium, evidence);
    expect(article && article->intent == cortex::NativeCognitiveIntent::long_form_writing
               && article->text.starts_with("# concorrência e estruturas de dados")
               && article->text.find("## Introdução") != std::string::npos
               && article->text.find("## Conclusão") != std::string::npos
               && article->text.size() > 1'000U,
           "especialista de redação longa organiza evidência em seções coerentes");

    const auto summary = cortex::synthesize_native_response(
        "Resuma: Testes verificam comportamento. Cobertura não prova correção. "
        "Casos de fronteira encontram defeitos. Uma suíte deve ser determinística.", {}, {}, medium);
    expect(summary && summary->intent == cortex::NativeCognitiveIntent::summarization
               && summary->text.find("Resumo:") != std::string::npos
               && summary->text.find("extrativo") != std::string::npos,
           "especialista de resumo seleciona frases sem inventar conteúdo");

    const auto code = cortex::synthesize_native_response(
        "Implemente código C++ para calcular fatorial com segurança", {}, {}, medium);
    expect(code && code->intent == cortex::NativeCognitiveIntent::programming
               && code->generated_code && code->text.find("std::optional") != std::string::npos
               && code->text.find("overflow") != std::string::npos,
           "especialista de programação gera C++20 completo com tratamento de overflow");

    const auto reasoning = cortex::synthesize_native_response(
        "Planeje como investigar um problema complexo com duas hipóteses", {}, {}, medium);
    expect(reasoning && reasoning->intent == cortex::NativeCognitiveIntent::reasoning
               && reasoning->text.find("Método verificável") != std::string::npos,
           "especialista de raciocínio explicita hipóteses, teste e atualização");
    const auto load_diagnosis = cortex::synthesize_native_response(
        "Planeje como investigar por que um servidor fica lento apenas sob carga", {}, {}, medium);
    expect(load_diagnosis && load_diagnosis->intent == cortex::NativeCognitiveIntent::reasoning
               && load_diagnosis->text.find("p50/p95/p99") != std::string::npos
               && load_diagnosis->text.find("Hipóteses concorrentes") != std::string::npos
               && load_diagnosis->text.find("Experimento em ordem causal") != std::string::npos,
           "raciocínio complexo especializa diagnóstico de servidor com hipóteses refutáveis");

    const auto conversation = cortex::synthesize_native_response(
        "O que você acha de aprender coisas novas?", {}, {}, medium);
    expect(conversation && conversation->intent == cortex::NativeCognitiveIntent::conversation
               && conversation->text.find("Não tenho opinião") != std::string::npos,
           "conversa aberta explora ideias sem fingir sentimentos ou opinião pessoal");

    const auto unsupported_fact = cortex::synthesize_native_response(
        "Qual é a capital desconhecida do planeta fictício Zorblax?", {}, {}, medium);
    expect(!unsupported_fact,
           "Cognitive Mesh não fabrica resposta factual quando não encontra evidência");

    const std::vector<float> first = cortex::native_feature_embedding(
        "compilador C++ com análise sintática", 128U);
    const std::vector<float> related = cortex::native_feature_embedding(
        "análise de compilador C++", 128U);
    const std::vector<float> unrelated = cortex::native_feature_embedding(
        "receita de bolo com chocolate", 128U);
    expect(cortex::dot_product(first, related) > cortex::dot_product(first, unrelated),
           "embedding lexical-hash aproxima termos relacionados sem pesos externos");

    const std::vector<std::string> chunks = cortex::chunk_knowledge_document(
        "manual.txt",
        "Primeiro parágrafo com conhecimento verificável.\n\n"
        "Segundo parágrafo descreve outro fato e sua aplicação prática.\n\n"
        "Terceiro parágrafo encerra o documento com uma conclusão.", 96U + 512U);
    expect(!chunks.empty() && chunks.front().starts_with("[DOC]")
               && std::all_of(chunks.begin(), chunks.end(), [](const std::string& chunk) {
                      return chunk.size() <= 608U;
                  }),
           "ingestão divide documentos, preserva origem e respeita limite de memória");
}

void test_open_model_profiles() {
    const cortex::OpenModelProfile x40 = cortex::open_model_profile("x40");
    const cortex::OpenModelProfile tiny = cortex::open_model_profile("tiny");
    const cortex::OpenModelProfile edge = cortex::open_model_profile("edge");
    const cortex::OpenModelProfile daily = cortex::open_model_profile("daily");
    const std::vector<cortex::OpenModelProfile> profiles = cortex::available_open_model_profiles();
    expect(std::none_of(profiles.begin(), profiles.end(), [](const auto& profile) {
               return profile.display_name.find("Qwen") != std::string::npos
                      || profile.hugging_face_reference.find("Qwen") != std::string::npos;
           }),
           "nenhum perfil principal depende de Qwen");
    expect(tiny.parameter_count == 2'000'000'000ULL
               && tiny.hugging_face_reference
                      == "ibm-granite/granite-3.3-2b-instruct-GGUF:Q3_K_M",
           "perfil tiny mantém Granite aberto em quantização reduzida");
    expect(x40.parameter_count == 3'000'000'000ULL
               && x40.hugging_face_reference
                      == "ibm-granite/granite-4.2-3b-GGUF:Q4_K_M"
               && x40.recommended_context == 4'096U
               && x40.recommended_threads == 4U
               && x40.recommended_gpu_layers == 0
               && x40.supports_reasoning_control,
           "perfil X40 usa Granite 4.2 multilíngue, raciocinador e CPU-only sem Qwen");
    expect(edge.parameter_count == 3'000'000'000ULL
               && edge.supports_reasoning_control,
           "perfil edge preserva raciocínio Granite em quantização menor");
    expect(daily.parameter_count == 8'000'000'000ULL
               && daily.hugging_face_reference
                      == "ibm-granite/granite-4.2-8b-GGUF:Q4_K_M",
           "perfil diário escala a mesma família aberta e reproduzível");
    expect(cortex::recommended_open_model_profile(2U).id == "tiny"
               && cortex::recommended_open_model_profile(4U).id == "edge"
               && cortex::recommended_open_model_profile(8U).id == "x40"
               && cortex::recommended_open_model_profile(16U).id == "daily"
               && cortex::recommended_open_model_profile(32U).id == "workstation"
               && cortex::recommended_open_model_profile(40U).id == "max",
           "seleção automática reserva RAM e prioriza Granite no notebook de 8 GiB");

    cortex::LocalModelOptions options;
    options.executable = "llama-server-test";
    options.hugging_face_reference = daily.hugging_face_reference;
    options.alias = "cortex-local";
    options.port = 18'081U;
    options.context_tokens = 8'192U;
    options.threads = 4U;
    options.gpu_layers = 99;
    const std::vector<std::string> command = cortex::local_model_arguments(options);
    const auto contains = [&](std::string_view value) {
        return std::any_of(command.begin(), command.end(), [&](const std::string& item) {
            return item == value;
        });
    };
    expect(contains("-hf") && contains(daily.hugging_face_reference),
           "launcher passa referência Hugging Face ao llama.cpp sem shell");
    expect(contains("127.0.0.1") && contains("18081"),
           "servidor GGUF fica restrito ao loopback em porta dedicada");
    expect(contains("8192") && contains("99") && contains("--jinja"),
           "launcher aplica contexto, chat template e offload configurados");

    bool rejected = false;
    try {
        static_cast<void>(cortex::open_model_profile("gemini-magico"));
    } catch (const cortex::Error&) {
        rejected = true;
    }
    expect(rejected, "perfil inexistente é rejeitado em vez de prometer parâmetros falsos");
}

void set_environment(const char* name, const char* value) {
#ifdef _WIN32
    if (_putenv_s(name, value) != 0) throw cortex::Error("falha ao configurar ambiente de teste");
#else
    if (setenv(name, value, 1) != 0) throw cortex::Error("falha ao configurar ambiente de teste");
#endif
}

void clear_environment(const char* name) {
#ifdef _WIN32
    static_cast<void>(_putenv_s(name, ""));
#else
    static_cast<void>(unsetenv(name));
#endif
}

void test_language_api_payload(const std::string& test_executable) {
    set_environment("CORTEX_TEST_FAKE_CURL", "1");
    set_environment("CORTEX_TEST_API_KEY", "chave-teste");

    cortex::LanguageApiConfig config;
    config.endpoint = "http://127.0.0.1:9999/v1/chat/completions";
    config.model = "gemini-test";
    config.api_key_environment = "CORTEX_TEST_API_KEY";
    config.reasoning_effort = "high";
    config.extended_sampling = true;
    config.top_p = 0.8F;
    config.presence_penalty = 1.5F;
    cortex::LanguageApiClient client(
        config, cortex::NativeHttpClient(test_executable));
    const cortex::LanguageCompletion completion = client.complete(
        "Seja direto.", "Explique C++ em uma linha.", 0.4F, 1'024U);

    clear_environment("CORTEX_TEST_FAKE_CURL");
    clear_environment("CORTEX_TEST_API_KEY");
    expect(completion.text == "payload Gemini válido",
           "cliente envia payload e Bearer por pipe, sem segredo exposto em argv");
    expect(completion.model == "gemini-test", "cliente preserva identidade do modelo externo");

    set_environment("CORTEX_TEST_FAKE_CURL", "1");
    set_environment("CORTEX_TEST_GRANITE", "1");
    cortex::LanguageApiConfig granite_config;
    granite_config.endpoint = "http://127.0.0.1:9999/v1/chat/completions";
    granite_config.model = "granite-test";
    granite_config.display_model = "Granite 4.2 teste";
    granite_config.api_key_environment.clear();
    granite_config.extended_sampling = true;
    granite_config.supports_reasoning_control = true;
    granite_config.requires_reasoning_sampling = true;
    cortex::LanguageApiClient granite(
        granite_config, cortex::NativeHttpClient(test_executable));
    const cortex::LanguageCompletion granite_completion = granite.complete(
        "Raciocine com privacidade.", "Resolva.", 0.2F, 512U, 8U,
        cortex::LanguageReasoningMode::full);
    set_environment("CORTEX_TEST_GRANITE_MODE", "low");
    const cortex::LanguageCompletion granite_low = granite.complete(
        "Seja eficiente.", "Resolva.", 0.2F, 512U, 8U,
        cortex::LanguageReasoningMode::low_effort);
    set_environment("CORTEX_TEST_GRANITE_MODE", "disabled");
    const cortex::LanguageCompletion granite_flash = granite.complete(
        "Seja rápido.", "Responda.", 0.2F, 256U, 8U,
        cortex::LanguageReasoningMode::disabled);
    clear_environment("CORTEX_TEST_GRANITE_MODE");
    clear_environment("CORTEX_TEST_GRANITE");
    clear_environment("CORTEX_TEST_FAKE_CURL");
    expect(granite_completion.text == "resposta Granite"
               && granite_completion.model == "Granite 4.2 teste"
               && granite_low.text == "resposta Granite"
               && granite_flash.text == "resposta Granite",
           "cliente mapeia High/Medium/Flash para full/low/off, força amostragem Granite e oculta trace");
    expect(granite_completion.reasoning == "trace privado"
               && granite_completion.prompt_tokens == 123U
               && granite_completion.generated_tokens == 45U,
           "cliente separa reasoning_content do texto e preserva contadores de uso do provedor");

    // Somente raciocínio, sem resposta final: precisa ser um erro específico, não
    // uma resposta vazia silenciosa.
    set_environment("CORTEX_TEST_FAKE_CURL", "1");
    set_environment("CORTEX_TEST_GRANITE", "1");
    set_environment("CORTEX_TEST_GRANITE_REASONING_ONLY", "1");
    bool reasoning_only_detected = false;
    std::string reasoning_only_message;
    try {
        static_cast<void>(granite.complete("Seja breve.", "Responda.", 1.0F, 256U, 8U,
                                           cortex::LanguageReasoningMode::disabled));
    } catch (const cortex::Error& error) {
        reasoning_only_message = error.what();
        reasoning_only_detected = reasoning_only_message.find("raciocínio") != std::string::npos;
    }
    clear_environment("CORTEX_TEST_GRANITE_REASONING_ONLY");
    clear_environment("CORTEX_TEST_GRANITE");
    clear_environment("CORTEX_TEST_FAKE_CURL");
    expect(reasoning_only_detected && !reasoning_only_message.empty(),
           "resposta apenas com raciocínio vira erro explicativo em vez de resposta vazia");

    config.reasoning_effort = "extreme";
    bool rejected = false;
    try {
        static_cast<void>(cortex::LanguageApiClient(config));
    } catch (const cortex::Error&) {
        rejected = true;
    }
    expect(rejected, "cliente rejeita reasoning_effort não suportado antes da rede");
}

void test_internet_search_fallback(const std::string& test_executable) {
    set_environment("CORTEX_TEST_FAKE_WEB", "duckduckgo-answer");
    const cortex::InternetSearch search{
        cortex::NativeHttpClient(test_executable)};
    const cortex::InternetResult primary = search.search("Ada Lovelace C++");
    const std::vector<cortex::InternetResult> multiple = search.search_multiple(
        "Ada Lovelace C++", 3U);
    clear_environment("CORTEX_TEST_FAKE_WEB");
    expect(primary.source_name == "DuckDuckGo"
               && primary.summary == "Resposta instantânea verificável"
               && primary.source_url == "https://example.org/fonte",
           "pesquisa usa resposta instantânea HTTPS do DuckDuckGo quando disponível");
    expect(multiple.size() == 2U
               && multiple[0].source_name == "DuckDuckGo"
               && multiple[1].source_name == "Wikipedia (pt)",
           "pesquisa de alta potência preserva evidências independentes de duas fontes");

    set_environment("CORTEX_TEST_FAKE_WEB", "wikipedia-fallback");
    const cortex::InternetResult result = search.search("Ada Lovelace C++");
    clear_environment("CORTEX_TEST_FAKE_WEB");

    expect(result.source_name == "Wikipedia (pt)",
           "pesquisa tenta Wikipedia quando DuckDuckGo não possui resposta");
    expect(result.heading == "Ada Lovelace"
               && result.summary.find("primeiro algoritmo") != std::string::npos,
           "fallback sem chave extrai título e resumo objetivo");
    expect(result.source_url == "https://pt.wikipedia.org/wiki/Ada_Lovelace",
           "pesquisa preserva somente a URL HTTPS da fonte");
}

void test_controlled_self_refinement(const fs::path& directory,
                                     const std::string& test_executable) {
    const fs::path root = directory / "self-project";
    const fs::path source_directory = root / "src";
    fs::create_directories(source_directory);
    const fs::path source_file = source_directory / "sample.cpp";
    const std::string original = "#include <cstdint>\n\nint answer() {\n    return 42;\n}\n";
    {
        std::ofstream output(source_file, std::ios::binary | std::ios::trunc);
        output << original;
    }

    set_environment("CORTEX_TEST_FAKE_CURL", "1");
    set_environment("CORTEX_TEST_SELF_EDIT", "1");
    cortex::LanguageApiConfig config;
    config.endpoint = "http://127.0.0.1:9999/v1/chat/completions";
    config.model = "cortex-self-test";
    config.api_key_environment.clear();
    cortex::LanguageApiClient language(
        config, cortex::NativeHttpClient(test_executable));
    cortex::SafeSelfEditor editor(root, language, true);

    bool controlled_creation_blocked = false;
    try {
        static_cast<void>(editor.propose_file(
            "src/new.cpp", "Crie um arquivo.", true));
    } catch (const cortex::Error&) {
        controlled_creation_blocked = true;
    }
    expect(controlled_creation_blocked,
           "modo padrão continua bloqueando criação fora do contrato controlado");

    const cortex::SourceAudit audit = editor.audit(
        "src/sample.cpp", "A função respondeu 42 quando o requisito passou a ser 43.");
    expect(audit.lines >= 5U && audit.maintainability_score > 80.0
               && !audit.suggested_instruction.empty(),
           "autoevolução começa por auditoria estática e comportamento observado");
    const cortex::CodeProposal proposal = editor.propose_refinement(
        "src/sample.cpp", "Atualize a resposta para 43.", "O teste agora espera 43.");
    expect(proposal.replacement.find("return 43") != std::string::npos
               && !proposal.audit_summary.empty(),
           "GGUF simulado produz proposta completa validada antes de aplicar");

    const cortex::AppliedCodeChange applied = editor.apply(
        proposal.id, "APLICAR " + std::to_string(proposal.id));
    std::ifstream changed_stream(source_file, std::ios::binary);
    const std::string changed((std::istreambuf_iterator<char>(changed_stream)),
                              std::istreambuf_iterator<char>());
    expect(changed.find("return 43") != std::string::npos
               && fs::is_regular_file(applied.backup_file),
           "aplicação confirmada cria backup e publica o fonte refinado");

    // Uma nova instância simula recompilação/reinício: o rollback deve sobreviver
    // ao desaparecimento do mapa de aplicações em memória.
    cortex::SafeSelfEditor restarted_editor(root, language, true);
    const cortex::RolledBackCodeChange rolled_back = restarted_editor.rollback(
        proposal.id, "REVERTER " + std::to_string(proposal.id));
    std::ifstream restored_stream(source_file, std::ios::binary);
    const std::string restored((std::istreambuf_iterator<char>(restored_stream)),
                               std::istreambuf_iterator<char>());
    expect(restored == original && rolled_back.restored_file == source_file,
           "rollback persistente restaura conteúdo exato mesmo após reinício");

    const fs::path external_file = directory / "system-area" / "created.cpp";
    cortex::SafeSelfEditor unrestricted(root, language, true, true);
    const cortex::ValidationProposal validation = unrestricted.stage_validation("echo validation-ok");
    bool validation_without_permission_blocked = false;
    try {
        static_cast<void>(unrestricted.run_validation(validation.id, "EXECUTAR 0"));
    } catch (const cortex::Error&) {
        validation_without_permission_blocked = true;
    }
    const cortex::ValidationResult validation_result = unrestricted.run_validation(
        validation.id, "EXECUTAR " + std::to_string(validation.id));
    expect(validation_without_permission_blocked && validation_result.exit_code == 0
               && validation_result.output.find("validation-ok") != std::string::npos,
           "validação livre mostra o comando e só executa após permissão de uso único");

    const cortex::CodeProposal creation = unrestricted.propose_file(
        external_file.string(), "Crie este arquivo C++ fora da raiz do projeto.", true);
    expect(creation.creates_file && creation.unrestricted_scope,
           "modo irrestrito pode propor arquivo novo em caminho absoluto");
    const cortex::AppliedCodeChange created = unrestricted.apply(
        creation.id, "APLICAR " + std::to_string(creation.id));
    expect(created.created_file && fs::is_regular_file(external_file),
           "arquivo externo só é criado depois da autorização exata");
    cortex::SafeSelfEditor restarted_unrestricted(root, language, true, true);
    const cortex::RolledBackCodeChange removed = restarted_unrestricted.rollback(
        creation.id, "REVERTER " + std::to_string(creation.id));
    expect(removed.restored_file == external_file && !fs::exists(external_file),
           "rollback persistente remove arquivo criado pelo modo irrestrito");

    // Sem GGUF nem API, o editor ainda executa transformações explícitas auditáveis.
    cortex::LanguageApiClient no_language_model(cortex::LanguageApiConfig{});
    cortex::SafeSelfEditor native_editor(root, no_language_model, true);
    const cortex::CodeProposal native_proposal = native_editor.propose(
        "src/sample.cpp", "Substitua `return 42;` por `return 44;`.");
    expect(native_proposal.replacement.find("return 44;") != std::string::npos
               && native_proposal.audit_summary.find("determinística nativa")
                      != std::string::npos,
           "autoedição nativa propõe substituição exata sem modelo de linguagem");
    const cortex::AppliedCodeChange native_applied = native_editor.apply(
        native_proposal.id, "APLICAR " + std::to_string(native_proposal.id));
    expect(fs::is_regular_file(native_applied.backup_file),
           "autoedição nativa preserva o mesmo ciclo de autorização e backup");
    static_cast<void>(native_editor.rollback(
        native_proposal.id, "REVERTER " + std::to_string(native_proposal.id)));

    set_environment("CORTEX_TEST_SELF_PATCH", "1");
    const cortex::CodeProposal patch_proposal = editor.propose(
        "src/sample.cpp", "Atualize a função answer para o novo requisito 45 com mudança mínima.");
    clear_environment("CORTEX_TEST_SELF_PATCH");
    expect(patch_proposal.replacement.find("return 45;") != std::string::npos
               && patch_proposal.replacement.find("#include <cstdint>") != std::string::npos,
           "autoeditor aplica patch SEARCH/REPLACE do Granite sem truncar o restante do arquivo");
    static_cast<void>(editor.apply(
        patch_proposal.id, "APLICAR " + std::to_string(patch_proposal.id)));
    static_cast<void>(editor.rollback(
        patch_proposal.id, "REVERTER " + std::to_string(patch_proposal.id)));

    {
        std::ofstream ambiguous(source_directory / "ambiguous.cpp", std::ios::binary);
        ambiguous << "int first() { return 1; }\nint second() { return 1; }\n";
    }
    set_environment("CORTEX_TEST_AMBIGUOUS_PATCH", "1");
    bool ambiguous_rejected = false;
    try {
        static_cast<void>(editor.propose(
            "src/ambiguous.cpp", "Atualize somente uma das funções conforme o requisito novo."));
    } catch (const cortex::Error&) {
        ambiguous_rejected = true;
    }
    clear_environment("CORTEX_TEST_AMBIGUOUS_PATCH");
    expect(ambiguous_rejected,
           "autoeditor rejeita SEARCH ambíguo em vez de alterar ocorrência arbitrária");

    const fs::path large_file = source_directory / "large.cpp";
    {
        std::ofstream output(large_file, std::ios::binary | std::ios::trunc);
        output << "#include <cstdint>\n";
        for (std::size_t index = 0U; index < 600U; ++index) {
            output << "// contexto anterior " << index << " para testar seleção de trecho\n";
        }
        output << "int target_function() {\n    return 1;\n}\n";
        for (std::size_t index = 0U; index < 600U; ++index) {
            output << "// contexto posterior " << index << " para testar seleção de trecho\n";
        }
    }
    set_environment("CORTEX_TEST_LARGE_PATCH", "1");
    const cortex::CodeProposal large_patch = editor.propose(
        "src/large.cpp", "Corrija target_function para devolver 2 sem alterar outras regiões.");
    clear_environment("CORTEX_TEST_LARGE_PATCH");
    expect(large_patch.replacement.find("return 2;") != std::string::npos
               && large_patch.replacement.size() > 40'000U,
           "autoeditor seleciona trecho relevante de arquivo grande e recompõe o arquivo completo");

    clear_environment("CORTEX_TEST_SELF_EDIT");
    clear_environment("CORTEX_TEST_FAKE_CURL");
}

void test_matrix() {
    cortex::Matrix matrix(2U, 3U);
    matrix(0U, 0U) = 1.0F;
    matrix(0U, 1U) = 2.0F;
    matrix(0U, 2U) = 3.0F;
    matrix(1U, 0U) = -1.0F;
    matrix(1U, 1U) = 0.5F;
    matrix(1U, 2U) = 2.0F;
    const std::vector<float> input{2.0F, -1.0F, 4.0F};
    std::vector<float> output(2U, 0.0F);
    matrix.matvec(input, output);
    expect(near(output[0], 12.0F), "produto matriz-vetor, linha zero");
    expect(near(output[1], 5.5F), "produto matriz-vetor, linha um");

    std::vector<float> transposed(3U, 0.0F);
    matrix.transposed_matvec(std::vector<float>{2.0F, 3.0F}, transposed);
    expect(near(transposed[0], -1.0F), "produto transposto, coluna zero");
    expect(near(transposed[1], 5.5F), "produto transposto, coluna um");
    expect(near(transposed[2], 12.0F), "produto transposto, coluna dois");
}

void test_tokenizer(const fs::path& directory) {
    const std::string text = "ação, banana e UTF-8: çã 🚀\nbanana banana";
    cortex::BpeTokenizer tokenizer;
    tokenizer.train(text, 48U, 2U);
    const std::vector<cortex::TokenId> encoded = tokenizer.encode(text, true, true);
    expect(encoded.front() == cortex::BpeTokenizer::bos_id, "BOS foi inserido");
    expect(encoded.back() == cortex::BpeTokenizer::eos_id, "EOS foi inserido");
    expect(tokenizer.decode(encoded) == text, "BPE reconstrói bytes UTF-8 exatamente");
    expect(tokenizer.merge_count() > 0U, "BPE aprendeu pelo menos um merge");

    const fs::path file = directory / "tokenizer.cbpe";
    tokenizer.save(file);
    const cortex::BpeTokenizer loaded = cortex::BpeTokenizer::load(file);
    expect(loaded.encode(text, true, true) == encoded, "checkpoint BPE preserva ranks");
    expect(loaded.decode(encoded) == text, "tokenizador carregado decodifica igualmente");
}

void test_neural_training_and_checkpoint(const fs::path& directory) {
    cortex::ModelConfig config;
    config.vocabulary_size = cortex::BpeTokenizer::first_merge_id;
    config.embedding_size = 8U;
    config.hidden_size = 12U;
    config.layer_count = 1U;
    config.seed = 12345U;
    cortex::NeuralLanguageModel model(config);

    // Linguagem sintética alterna dois tokens; uma GRU deve aprender a transição rapidamente.
    std::vector<cortex::TokenId> sequence;
    sequence.push_back(cortex::BpeTokenizer::bos_id);
    for (std::size_t index = 0U; index < 96U; ++index) {
        sequence.push_back(index % 2U == 0U ? 10U : 11U);
    }
    sequence.push_back(cortex::BpeTokenizer::eos_id);

    const float before = model.evaluate_loss(sequence);
    cortex::TrainingOptions options;
    options.epochs = 18U;
    options.sequence_length = 16U;
    options.learning_rate = 0.02F;
    options.gradient_clip = 5.0F;
    const std::vector<cortex::TrainingMetrics> history = model.train(sequence, options);
    const float after = model.evaluate_loss(sequence);
    expect(history.size() == options.epochs, "histórico contém todas as épocas");
    expect(std::isfinite(after), "loss treinada é finita");
    expect(after < before * 0.45F, "BPTT e Adam reduzem significativamente a loss");

    cortex::NeuralLanguageModel::State original_state = model.zero_state();
    const std::vector<float> original_logits = model.step(10U, original_state);
    const fs::path file = directory / "model.cgru";
    model.save(file);
    cortex::NeuralLanguageModel loaded = cortex::NeuralLanguageModel::load(file);
    cortex::NeuralLanguageModel::State loaded_state = loaded.zero_state();
    const std::vector<float> loaded_logits = loaded.step(10U, loaded_state);
    expect(original_logits == loaded_logits, "checkpoint preserva pesos float bit a bit");
    expect(loaded.optimizer_step() == model.optimizer_step(),
           "checkpoint preserva passo e momentos do Adam");

    cortex::GenerationOptions generation;
    generation.maximum_tokens = 8U;
    generation.top_k = 2U;
    generation.temperature = 0.5F;
    generation.seed = 7U;
    const std::vector<cortex::TokenId> first = model.generate(
        std::vector<cortex::TokenId>{10U}, generation);
    const std::vector<cortex::TokenId> second = loaded.generate(
        std::vector<cortex::TokenId>{10U}, generation);
    expect(first == second, "amostragem com mesma semente é determinística");
}

void test_direct_answer_extraction() {
    expect(cortex::extract_direct_answer(
               "  Assistente: Resposta objetiva.\n\nUsuário: próxima pergunta")
               == "Resposta objetiva.",
           "agente remove papel e interrompe antes do próximo usuário");
    expect(cortex::extract_direct_answer("Resposta curta.\nSistema: novo turno")
               == "Resposta curta.",
           "agente interrompe antes de uma continuação de sistema");
    expect(cortex::extract_direct_answer("Cortex: texto sem marcador posterior  ")
               == "texto sem marcador posterior",
           "agente remove prefixo Cortex e espaços finais");
    expect(cortex::extract_direct_answer("\n\t") == "",
           "saída neural sem conteúdo permanece vazia para fallback");
}

void test_native_reasoner() {
    cortex::ModelConfig config;
    config.vocabulary_size = cortex::BpeTokenizer::first_merge_id;
    config.embedding_size = 8U;
    config.hidden_size = 12U;
    config.layer_count = 1U;
    config.seed = 77U;
    const cortex::NeuralLanguageModel model(config);

    const cortex::WebResearchRequest research = cortex::parse_web_research_request(
        "Pesquise na internet por: Ada Lovelace e C++  ");
    expect(research.explicit_request && research.query == "Ada Lovelace e C++",
           "parser reconhece pesquisa explícita e preserva a consulta original");
    const cortex::WebResearchRequest empty_research =
        cortex::parse_web_research_request("pesquise na internet por:");
    expect(empty_research.explicit_request && empty_research.query.empty(),
           "parser distingue comando de pesquisa sem assunto");
    const cortex::WebResearchRequest unicode_research =
        cortex::parse_web_research_request("busque na internet por 🚀");
    expect(unicode_research.explicit_request && unicode_research.query == "🚀",
           "parser mantém consulta UTF-8 mesmo sem caracteres latinos");
    expect(!cortex::parse_web_research_request("explique a internet").explicit_request,
           "menção comum à internet não ativa pesquisa explícita");

    const auto text_analysis = cortex::reason_locally(
        "analise este texto: Uma frase clara. Outra frase verificável.", {}, model);
    expect(text_analysis && text_analysis->mode == cortex::LocalResponseMode::text_analysis
               && text_analysis->text.find("Análise textual determinística") != std::string::npos,
           "reasoner analisa texto localmente antes de qualquer geração");
    const auto self_analysis = cortex::reason_locally(
        "analise sua resposta",
        "Usuário: Explique RAII em C++\nAssistente: RAII em C++ associa recursos a objetos.",
        model);
    expect(self_analysis && self_analysis->mode == cortex::LocalResponseMode::self_analysis
               && self_analysis->text.find("Autoanálise da resposta anterior") != std::string::npos,
           "reasoner avalia o próprio comportamento recente com limites explícitos");
    const auto curated = cortex::reason_locally(
        "Como funcionam índices de banco de dados SQL?", {}, model);
    expect(curated && curated->mode == cortex::LocalResponseMode::curated_knowledge
               && curated->text.find("B-tree") != std::string::npos,
           "reasoner consulta base geral curada com foco em programação");
    expect(!cortex::reason_locally(
               "Redija um artigo longo sobre concorrência e estruturas de dados", {}, model)
               && !cortex::reason_locally(
                   "Implemente código C++ para fatorial com segurança", {}, model),
           "reasoner não deixa palavra temática curta interceptar composição ou programação");

    const auto benchmark = cortex::reason_locally("benchmark", {}, model);
    expect(benchmark && benchmark->mode == cortex::LocalResponseMode::benchmark
               && benchmark->text.find("Benchmark local concluído") != std::string::npos
               && benchmark->text.find("passos neurais/s") != std::string::npos,
           "reasoner executa benchmark real em vez de responder com saudação");

    cortex::BpeTokenizer tokenizer;
    cortex::LongTermMemory agent_memory(config.hidden_size);
    cortex::CognitiveAgent agent(tokenizer, model, agent_memory);
    cortex::AgentOptions agent_options;
    agent_options.remember_interaction = false;
    const cortex::AgentReply integrated = agent.respond("benchmark", agent_options);
    expect(integrated.response_mode == cortex::LocalResponseMode::benchmark
               && integrated.generated_tokens.empty()
               && integrated.text.find("Benchmark local concluído") != std::string::npos,
           "CognitiveAgent prioriza ferramenta nativa e não amostra o GRU para benchmark");
    const cortex::AgentReply research_without_transport = agent.respond(
        "pesquise na internet por Ada Lovelace", agent_options);
    expect(research_without_transport.response_mode == cortex::LocalResponseMode::safe_fallback
               && research_without_transport.generated_tokens.empty()
               && research_without_transport.text.find("Reconheci o pedido de pesquisa")
                      != std::string::npos,
           "agente reconhece intenção web antes do GRU quando não possui transporte HTTP");
    agent_options.generation.maximum_tokens = 24U;
    agent_options.maximum_context_tokens = 64U;
    const cortex::AgentReply unknown = agent.respond(
        "capital desconhecida de um planeta fictício", agent_options);
    expect(unknown.response_mode == cortex::LocalResponseMode::safe_fallback
               && unknown.generated_tokens.empty()
               && unknown.text.find("sem inventar") != std::string::npos,
           "CognitiveAgent descarta geração sem evidência e assume seu limite");

    const auto calculation = cortex::reason_locally("Quanto é 2 * (8 + 3)?", {}, model);
    expect(calculation && calculation->mode == cortex::LocalResponseMode::calculation
               && calculation->text.find("= 22") != std::string::npos,
           "reasoner calcula expressão com precedência e parênteses");

    const auto invalid_calculation = cortex::reason_locally("calcule 9 / 0", {}, model);
    expect(invalid_calculation
               && invalid_calculation->text.find("divisão por zero") != std::string::npos,
           "calculadora rejeita divisão por zero com explicação lógica");

    const auto architecture = cortex::reason_locally(
        "Como BPE, GRU e memória trabalham juntos?", {}, model);
    expect(architecture && architecture->mode == cortex::LocalResponseMode::knowledge
               && architecture->text.find("sete caminhos") != std::string::npos,
           "base técnica responde por intenção, não por próxima palavra aleatória");

    const auto follow_up = cortex::reason_locally(
        "explique mais",
        "Usuário: Como funciona uma GRU?\nAssistente: Um GGUF também pode ser usado.", model);
    expect(follow_up && follow_up->text.find("gate de reset") != std::string::npos,
           "continuação usa a última pergunta do usuário, não palavras da resposta anterior");

    const std::array<cortex::MemoryHit, 1U> memories{{
        cortex::MemoryHit{42U, 0U, 0.91F, "Usuário: meu editor favorito é Vim"}
    }};
    const auto memory = cortex::reason_locally(
        "O que você lembra de mim?", {}, model, memories);
    expect(memory && memory->mode == cortex::LocalResponseMode::memory
               && memory->text.find("Vim") != std::string::npos,
           "consulta explícita devolve memória persistente recuperada");

    const std::array<cortex::MemoryHit, 2U> web_memories{{
        cortex::MemoryHit{7U, 0U, 0.93F,
            "[WEB]\nConsulta: Ada Lovelace\nFonte: Wikipedia (pt)\n"
            "URL: https://pt.wikipedia.org/wiki/Ada_Lovelace\n"
            "Conteúdo: Ada Lovelace publicou o primeiro algoritmo."},
        cortex::MemoryHit{8U, 0U, 0.99F, "Usuário: texto sem procedência web"}
    }};
    const auto learned_web = cortex::reason_locally(
        "Quem foi Ada Lovelace?", {}, model, web_memories);
    expect(learned_web && learned_web->mode == cortex::LocalResponseMode::web_knowledge
               && learned_web->text.find("primeiro algoritmo") != std::string::npos
               && learned_web->text.find("Wikipedia (pt)") != std::string::npos,
           "reasoner reutiliza pesquisa persistida como conhecimento offline fundamentado");
    expect(cortex::local_response_mode_name(cortex::LocalResponseMode::web_knowledge)
               == "local-web-knowledge"
               && cortex::local_response_mode_name(cortex::LocalResponseMode::text_analysis)
                      == "local-text-analysis"
               && cortex::local_response_mode_name(cortex::LocalResponseMode::curated_knowledge)
                      == "local-curated-knowledge",
           "respostas identificam memória web, análise e base curada na interface");

    expect(!cortex::neural_response_is_relevant(
               "benchmark", "Olá! Estou pronta para responder. Como posso ajudar?"),
           "filtro rejeita exatamente a resposta desconectada relatada pelo usuário");
    expect(cortex::neural_response_is_relevant(
               "Como funciona uma GRU?",
               "A GRU funciona com gates que atualizam seu estado oculto."),
           "filtro preserva resposta neural com termos relevantes");
    const auto model_identity = cortex::reason_locally("Qual é o seu modelo?", {}, model);
    expect(model_identity
               && model_identity->text.find(std::to_string(model.parameter_count()))
                      != std::string::npos,
           "reasoner explica o modelo realmente carregado sem solicitar uma API");
    const auto stable_fact = cortex::reason_locally("Por que o céu é azul?", {}, model);
    expect(stable_fact && stable_fact->text.find("Rayleigh") != std::string::npos,
           "base local fornece fato estável com explicação causal");
    expect(!cortex::reason_locally("capital desconhecida de um planeta fictício", {}, model),
           "reasoner não inventa conhecimento fora da base verificável");
    expect(cortex::safe_local_fallback().text.find("sem inventar") != std::string::npos,
           "fallback admite limite local em vez de alucinar");
}

void test_embedded_web_interface() {
    const std::string_view html = cortex::web_chat_html();
    expect(html.starts_with("<!doctype html>"), "interface web possui documento HTML completo");
    expect(html.find("/api/chat") != std::string_view::npos,
           "interface web conversa com endpoint relativo nativo");
    expect(html.find("Node.js") != std::string_view::npos,
           "interface informa ausência de runtime Node.js");
    expect(html.find("language_api") != std::string_view::npos,
           "interface permite ativar o modelo GGUF local sem expor uma chave");
    expect(html.find("local-open-model") != std::string_view::npos,
           "interface identifica execução GGUF realmente local");
    expect(html.find("Aprender programação") != std::string_view::npos
               && html.find("Analisar um texto") != std::string_view::npos
               && html.find("Cognitive Mesh") != std::string_view::npos,
           "interface oferece conhecimento curado, análise e síntese cognitiva");
    expect(html.find("/api/knowledge/ingest") != std::string_view::npos
               && html.find("Ensinar documento") != std::string_view::npos,
           "interface permite aprendizado documental persistente sem API");
    expect(html.find("Qwen3 1.7B") == std::string_view::npos,
           "interface principal não recomenda mais Qwen");
    expect(html.find("powerMode") != std::string_view::npos
               && html.find("Alta potência · revisão dupla") != std::string_view::npos,
           "interface expõe modos Flash, Medium e Alta potência");
    expect(html.find("pesquise na internet por computação quântica") != std::string_view::npos
               && html.find("salvo para uso offline") != std::string_view::npos,
           "interface ensina a pesquisa sem chave e sinaliza aprendizado persistente");
    expect(html.find("id=\"remember\" type=\"checkbox\" checked")
               == std::string_view::npos,
           "memória automática inicia desligada para evitar episódios irrelevantes");
    expect(html.find("/api/code/audit") != std::string_view::npos
               && html.find("/api/code/refine") != std::string_view::npos
               && html.find("/api/code/evolve") != std::string_view::npos
               && html.find("Sugerir evolução automática") != std::string_view::npos
               && html.find("/api/code/rollback") != std::string_view::npos,
           "interface possui evolução automática por telemetria, autorrefinamento e rollback");
    expect(html.find("/api/validation/stage") != std::string_view::npos
               && html.find("/api/validation/run") != std::string_view::npos
               && html.find("Criar se não existir") != std::string_view::npos,
           "interface irrestrita prepara criação e validações com permissão explícita");
}

void test_long_term_memory(const fs::path& directory) {
    cortex::LongTermMemory memory(3U);
    const std::uint64_t alpha = memory.remember("alfa", std::vector<float>{1.0F, 0.0F, 0.0F});
    const std::uint64_t duplicate_alpha = memory.remember(
        "alfa", std::vector<float>{1.0F, 0.0F, 0.0F});
    const std::uint64_t beta = memory.remember("beta", std::vector<float>{0.0F, 1.0F, 0.0F});
    expect(duplicate_alpha == alpha && memory.size() == 2U,
           "memória deduplica conteúdo idêntico durante ingestões repetidas");
    expect(alpha != beta, "ids de memória são únicos");

    const std::vector<cortex::MemoryHit> hits = memory.search(
        std::vector<float>{0.9F, 0.1F, 0.0F}, 2U);
    expect(hits.size() == 2U, "busca retorna limite solicitado");
    expect(hits.front().id == alpha, "cosseno ordena o vetor semanticamente mais próximo");
    expect(hits.front().score > hits.back().score, "scores possuem ordem decrescente");

    const std::uint64_t document = memory.remember(
        "[DOC]\nFonte: história\nConteúdo: Ada Lovelace escreveu um algoritmo para a máquina analítica.",
        std::vector<float>{0.0F, 0.0F, 1.0F});
    const std::vector<cortex::MemoryHit> hybrid = memory.hybrid_search(
        "Quem foi Ada Lovelace?", std::vector<float>{1.0F, 0.0F, 0.0F}, 1U, 0.4F);
    expect(!hybrid.empty() && hybrid.front().id == document,
           "busca híbrida recupera nomes exatos mesmo quando o vetor neural diverge");

    const fs::path file = directory / "memory.cmem";
    memory.save(file);
    cortex::LongTermMemory loaded = cortex::LongTermMemory::load(file, 3U);
    expect(loaded.size() == 3U, "persistência conserva todos os episódios e documentos");
    const std::vector<cortex::MemoryHit> loaded_hits = loaded.search(
        std::vector<float>{0.0F, 1.0F, 0.0F}, 1U);
    expect(loaded_hits.front().id == beta, "índices persistidos mantêm semântica");
    expect(loaded.erase(alpha), "remoção encontra id existente");
    expect(!loaded.erase(alpha), "remoção sinaliza id já ausente");
}

} // namespace

void test_executable_resolution(const fs::path& directory) {
    // Nome sem separador continua dependendo do PATH: reescrevê-lo mudaria a
    // semântica de busca do Windows.
    expect(cortex::resolve_executable_path("curl") == "curl",
           "resolução preserva binário procurado no PATH");
    expect(cortex::resolve_executable_path("llama-server") == "llama-server",
           "resolução preserva llama-server sem extensão");

    const fs::path tool = directory / "cortex-fake-tool";
    {
        std::ofstream stream(tool);
        stream << "binario de teste";
    }
    expect(cortex::resolve_executable_path(tool.string()) == tool.string(),
           "resolução preserva caminho existente");

#ifdef _WIN32
    // Regressão da falha do CI MinGW: `make test` executa ./build/cortex-tests,
    // o argv[0] chega sem `.exe` e o subprocesso não era encontrado.
    const fs::path extensionless = directory / "cortex-fake-tool-sem-ext";
    const fs::path with_extension = directory / "cortex-fake-tool-sem-ext.exe";
    {
        std::ofstream stream(with_extension);
        stream << "binario de teste";
    }
    expect(cortex::resolve_executable_path(extensionless.string()) == with_extension.string(),
           "resolução acrescenta .exe quando o shell do MinGW omite a extensão");
#endif

    bool rejected = false;
    try {
        static_cast<void>(cortex::resolve_executable_path(""));
    } catch (const cortex::Error&) {
        rejected = true;
    }
    expect(rejected, "resolução rejeita executável vazio");
}

int main(int argc, char** argv) {
    // O próprio binário também simula as duas fontes HTTPS. DuckDuckGo devolve
    // JSON vazio e a segunda chamada só responde se a consulta chegou codificada.
    if (const char* fake_web = std::getenv("CORTEX_TEST_FAKE_WEB"); fake_web != nullptr) {
        const std::string_view mode(fake_web);
        std::string url;
        for (int index = 1; index + 1 < argc; ++index) {
            if (std::string_view(argv[index]) == "--url") {
                url = argv[index + 1];
                break;
            }
        }
        if (url.find("api.duckduckgo.com") != std::string::npos) {
            if (mode == "duckduckgo-answer") {
                std::cout << R"({"Heading":"Ada Lovelace","AbstractText":"Resposta instantânea verificável","AbstractURL":"https://example.org/fonte"})"
                          << "\n__CORTEX_HTTP_STATUS__:200";
            } else {
                std::cout << "{\"Heading\":\"\",\"AbstractText\":\"\",\"Answer\":\"\"}"
                             "\n__CORTEX_HTTP_STATUS__:200";
            }
            return 0;
        }
        const bool wikipedia = url.find("pt.wikipedia.org/w/api.php") != std::string::npos;
        const bool encoded_query = url.find("Ada%20Lovelace%20C%2B%2B") != std::string::npos;
        if (wikipedia && encoded_query) {
            std::cout << R"({"query":{"pages":[{"pageid":123,"title":"Ada Lovelace","extract":"Ada Lovelace publicou o primeiro algoritmo destinado a uma máquina.","fullurl":"https://pt.wikipedia.org/wiki/Ada_Lovelace"}]}})"
                      << "\n__CORTEX_HTTP_STATUS__:200";
            return 0;
        }
        std::cout << "{}\n__CORTEX_HTTP_STATUS__:404";
        return 0;
    }

    // O próprio binário simula curl em um subprocesso: nenhum servidor ou script externo
    // é necessário para validar o JSON OpenAI-compatible e o header de autenticação.
    if (std::getenv("CORTEX_TEST_FAKE_CURL") != nullptr) {
        std::ostringstream input;
        input << std::cin.rdbuf();
        const std::string request = input.str();
        if (std::getenv("CORTEX_TEST_GRANITE") != nullptr) {
            const std::string mode = std::getenv("CORTEX_TEST_GRANITE_MODE") == nullptr
                ? "full" : std::getenv("CORTEX_TEST_GRANITE_MODE");
            const bool expected_thinking = mode != "disabled";
            const bool expected_low_effort = mode == "low";
            const bool valid = request.find("\\\"model\\\":\\\"granite-test\\\"")
                                   != std::string::npos
                               && request.find("\\\"temperature\\\":1")
                                      != std::string::npos
                               && request.find("\\\"top_p\\\":0.95")
                                      != std::string::npos
                               && request.find(expected_thinking
                                      ? "\\\"enable_thinking\\\":true"
                                      : "\\\"enable_thinking\\\":false") != std::string::npos
                               && request.find(expected_low_effort
                                      ? "\\\"low_effort\\\":true"
                                      : "\\\"low_effort\\\":false") != std::string::npos;
            if (std::getenv("CORTEX_TEST_GRANITE_REASONING_ONLY") != nullptr) {
                // Cenário real: o orçamento acaba dentro de <think> e o provedor
                // devolve apenas reasoning_content, sem resposta final.
                std::cout << "{\"choices\":[{\"message\":{\"content\":\"\","
                             "\"reasoning_content\":\"pensando sem concluir\"}}],"
                             "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":256}}\n"
                             "__CORTEX_HTTP_STATUS__:200";
                return 0;
            }
            const std::string content = valid
                ? "<think>raciocínio privado</think> resposta Granite"
                : "payload Granite inválido";
            std::cout << "{\"choices\":[{\"message\":{\"content\":"
                      << cortex::json_quote(content)
                      << ",\"reasoning_content\":\"trace privado\"}}],"
                         "\"usage\":{\"prompt_tokens\":123,\"completion_tokens\":45,"
                         "\"total_tokens\":168}}\n__CORTEX_HTTP_STATUS__:200";
            return 0;
        }
        if (std::getenv("CORTEX_TEST_SELF_EDIT") != nullptr) {
            std::string replacement;
            if (std::getenv("CORTEX_TEST_LARGE_PATCH") != nullptr) {
                const bool excerpt_selected = request.find("target_function") != std::string::npos
                                              && request.size() < 20'000U;
                replacement = excerpt_selected
                    ? "<<<<<<< SEARCH\nint target_function() {\n    return 1;\n}\n=======\n"
                      "int target_function() {\n    return 2;\n}\n>>>>>>> REPLACE"
                    : "patch inválido";
            } else if (std::getenv("CORTEX_TEST_AMBIGUOUS_PATCH") != nullptr) {
                replacement = "<<<<<<< SEARCH\nreturn 1;\n=======\nreturn 2;\n>>>>>>> REPLACE";
            } else if (std::getenv("CORTEX_TEST_SELF_PATCH") != nullptr) {
                replacement = "<<<<<<< SEARCH\nint answer() {\n    return 42;\n}\n=======\n"
                              "int answer() {\n    return 45;\n}\n>>>>>>> REPLACE";
            } else {
                replacement = "```cpp\n#include <cstdint>\n\nint answer() {\n    return 43;\n}\n```";
            }
            std::cout << "{\"choices\":[{\"message\":{\"content\":"
                      << cortex::json_quote(replacement)
                      << "}}]}\n__CORTEX_HTTP_STATUS__:200";
            return 0;
        }
        bool bearer_in_arguments = false;
        for (int index = 1; index < argc; ++index) {
            if (std::string_view(argv[index]).find("Authorization: Bearer chave-teste")
                != std::string_view::npos) {
                bearer_in_arguments = true;
            }
        }
        const bool bearer_in_pipe = request.find("Authorization: Bearer chave-teste")
                                    != std::string::npos;
        const bool explicit_payload = request.find("\\\"model\\\":\\\"gemini-test\\\"")
                                          != std::string::npos
                                      && request.find("\\\"reasoning_effort\\\":\\\"high\\\"")
                                             != std::string::npos
                                      && request.find("\\\"top_k\\\":20") != std::string::npos
                                      && request.find("\\\"top_p\\\":0.8") != std::string::npos
                                      && request.find("\\\"presence_penalty\\\":1.5")
                                             != std::string::npos;
        const bool payload_valid = request.find("\\\"max_tokens\\\":1024")
                                       != std::string::npos
                                   && explicit_payload;
        const char* content = bearer_in_pipe && !bearer_in_arguments && payload_valid
                              ? "payload Gemini válido" : "payload inválido";
        std::cout << "{\"choices\":[{\"message\":{\"content\":"
                  << cortex::json_quote(content)
                  << "}}]}\n__CORTEX_HTTP_STATUS__:200";
        return 0;
    }

    const fs::path directory = temporary_test_directory();
    try {
        test_connectivity_protocol();
        test_quality_knowledge_and_power_modes();
        test_native_cognitive_mesh();
        test_open_model_profiles();
        test_executable_resolution(directory);
        test_language_api_payload(argc > 0 ? argv[0] : "./build/cortex-tests");
        test_internet_search_fallback(argc > 0 ? argv[0] : "./build/cortex-tests");
        test_controlled_self_refinement(
            directory, argc > 0 ? argv[0] : "./build/cortex-tests");
        test_matrix();
        test_tokenizer(directory);
        test_neural_training_and_checkpoint(directory);
        test_direct_answer_extraction();
        test_native_reasoner();
        test_embedded_web_interface();
        test_long_term_memory(directory);
        fs::remove_all(directory);
        std::cout << "OK — " << passed << " verificações passaram.\n";
        return 0;
    } catch (const std::exception& exception) {
        fs::remove_all(directory);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
