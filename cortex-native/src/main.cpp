#include "cortex/agent.hpp"
#include "cortex/core.hpp"
#include "cortex/local_model.hpp"
#include "cortex/memory.hpp"
#include "cortex/neural_model.hpp"
#include "cortex/tokenizer.hpp"
#include "cortex/web_server.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cortex::Error;
namespace fs = std::filesystem;

class Arguments final {
public:
    Arguments(int argc, char** argv, int first) {
        for (int index = first; index < argc; ++index) {
            std::string item = argv[index];
            if (item.rfind("--", 0U) != 0U) {
                positional_.push_back(std::move(item));
                continue;
            }
            item.erase(0U, 2U);
            const std::size_t equals = item.find('=');
            if (equals != std::string::npos) {
                values_[item.substr(0U, equals)] = item.substr(equals + 1U);
            } else if (index + 1 < argc
                       && std::string_view(argv[index + 1]).rfind("--", 0U) != 0U) {
                values_[item] = argv[++index];
            } else {
                values_[item] = "true";
            }
        }
    }

    [[nodiscard]] std::string get(std::string_view key,
                                  std::string fallback = {}) const {
        const auto found = values_.find(std::string(key));
        return found == values_.end() ? std::move(fallback) : found->second;
    }

    [[nodiscard]] bool has(std::string_view key) const {
        return values_.contains(std::string(key));
    }

    [[nodiscard]] bool flag(std::string_view key, bool fallback = false) const {
        if (!has(key)) {
            return fallback;
        }
        const std::string value = get(key);
        if (value == "true" || value == "1" || value == "sim" || value == "yes") {
            return true;
        }
        if (value == "false" || value == "0" || value == "nao" || value == "não"
            || value == "no") {
            return false;
        }
        throw Error("valor booleano inválido para --" + std::string(key));
    }

    [[nodiscard]] std::size_t size_value(std::string_view key,
                                         std::size_t fallback) const {
        if (!has(key)) {
            return fallback;
        }
        const std::string value = get(key);
        std::size_t consumed = 0U;
        const unsigned long long parsed = std::stoull(value, &consumed, 0);
        if (consumed != value.size()) {
            throw Error("inteiro inválido para --" + std::string(key));
        }
        return static_cast<std::size_t>(parsed);
    }

    [[nodiscard]] std::uint64_t u64_value(std::string_view key,
                                          std::uint64_t fallback) const {
        if (!has(key)) {
            return fallback;
        }
        const std::string value = get(key);
        std::size_t consumed = 0U;
        const unsigned long long parsed = std::stoull(value, &consumed, 0);
        if (consumed != value.size()) {
            throw Error("inteiro inválido para --" + std::string(key));
        }
        return static_cast<std::uint64_t>(parsed);
    }

    [[nodiscard]] int int_value(std::string_view key, int fallback) const {
        if (!has(key)) {
            return fallback;
        }
        const std::string value = get(key);
        std::size_t consumed = 0U;
        const int parsed = std::stoi(value, &consumed, 0);
        if (consumed != value.size()) {
            throw Error("inteiro inválido para --" + std::string(key));
        }
        return parsed;
    }

    [[nodiscard]] float float_value(std::string_view key, float fallback) const {
        if (!has(key)) {
            return fallback;
        }
        const std::string value = get(key);
        std::size_t consumed = 0U;
        const float parsed = std::stof(value, &consumed);
        if (consumed != value.size()) {
            throw Error("decimal inválido para --" + std::string(key));
        }
        return parsed;
    }

    [[nodiscard]] std::string joined_positional() const {
        std::ostringstream joined;
        for (std::size_t index = 0U; index < positional_.size(); ++index) {
            if (index != 0U) {
                joined << ' ';
            }
            joined << positional_[index];
        }
        return joined.str();
    }

private:
    std::map<std::string, std::string, std::less<>> values_;
    std::vector<std::string> positional_;
};

struct StatePaths final {
    fs::path directory;
    fs::path tokenizer;
    fs::path model;
    fs::path memory;
};

[[nodiscard]] std::string environment_value(const char* name,
                                             std::string fallback = {}) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string(value) : std::move(fallback);
}

[[nodiscard]] std::optional<fs::path> executable_on_path(std::string executable) {
    if (executable.empty()) return std::nullopt;
#ifdef _WIN32
    if (fs::path(executable).extension().empty()) executable += ".exe";
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    const fs::path direct(executable);
    if (direct.has_parent_path() && fs::is_regular_file(direct)) return direct;
    const std::string path = environment_value("PATH");
    std::size_t begin = 0U;
    while (begin <= path.size()) {
        const std::size_t end = path.find(separator, begin);
        const std::string_view directory(path.data() + begin,
                                         (end == std::string::npos ? path.size() : end) - begin);
        if (!directory.empty()) {
            const fs::path candidate = fs::path(std::string(directory)) / executable;
            std::error_code error;
            if (fs::is_regular_file(candidate, error)) return candidate;
        }
        if (end == std::string::npos) break;
        begin = end + 1U;
    }
    return std::nullopt;
}

[[nodiscard]] StatePaths state_paths(const Arguments& arguments) {
    const fs::path directory = arguments.get("state", "state");
    return StatePaths{
        directory,
        directory / "tokenizer.cbpe",
        directory / "model.cgru",
        directory / "memory.cmem"
    };
}

[[nodiscard]] std::string read_text_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw Error("não foi possível abrir corpus: " + path.string());
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff length = stream.tellg();
    if (length <= 0 || length > static_cast<std::streamoff>(1024LL * 1024LL * 1024LL)) {
        throw Error("corpus vazio, inválido ou maior que 1 GiB");
    }
    stream.seekg(0, std::ios::beg);
    std::string content(static_cast<std::size_t>(length), '\0');
    stream.read(content.data(), length);
    if (!stream) {
        throw Error("falha durante leitura do corpus");
    }
    return content;
}

struct Runtime final {
    cortex::BpeTokenizer tokenizer;
    cortex::NeuralLanguageModel model;
    cortex::LongTermMemory memory;
};

[[nodiscard]] Runtime load_runtime(const StatePaths& paths) {
    cortex::BpeTokenizer tokenizer = cortex::BpeTokenizer::load(paths.tokenizer);
    cortex::NeuralLanguageModel model = cortex::NeuralLanguageModel::load(paths.model);
    if (tokenizer.vocabulary_size() != model.config().vocabulary_size) {
        throw Error("tokenizador salvo não corresponde ao modelo");
    }
    cortex::LongTermMemory memory = fs::exists(paths.memory)
        ? cortex::LongTermMemory::load(paths.memory, model.config().hidden_size)
        : cortex::LongTermMemory(model.config().hidden_size);
    return Runtime{std::move(tokenizer), std::move(model), std::move(memory)};
}

[[nodiscard]] cortex::GenerationOptions generation_options(const Arguments& arguments) {
    cortex::GenerationOptions options;
    options.maximum_tokens = arguments.size_value("tokens", options.maximum_tokens);
    options.top_k = arguments.size_value("top-k", options.top_k);
    options.temperature = arguments.float_value("temperature", options.temperature);
    options.repetition_penalty = arguments.float_value("repetition", options.repetition_penalty);
    options.seed = arguments.u64_value("seed", options.seed);
    return options;
}

[[nodiscard]] cortex::AgentOptions agent_options(const Arguments& arguments) {
    cortex::AgentOptions options;
    const std::string requested_power = arguments.get(
        "power", arguments.get("power-mode", "medium"));
    options.power_mode = cortex::parse_power_mode(requested_power);
    const cortex::PowerProfile& profile = cortex::power_profile(options.power_mode);
    options.memory_hits = arguments.size_value("memory-hits", profile.memory_hits);
    options.minimum_memory_score = arguments.float_value(
        "memory-score", options.power_mode == cortex::PowerMode::flash ? 0.70F
                        : options.power_mode == cortex::PowerMode::high ? 0.55F : 0.65F);
    options.maximum_context_tokens = arguments.size_value("context", profile.context_tokens);
    options.remember_interaction = !arguments.flag("no-remember", false);
    options.generation.maximum_tokens = arguments.size_value("tokens", profile.maximum_tokens);
    options.generation.top_k = arguments.size_value("top-k", profile.top_k);
    options.generation.temperature = arguments.float_value("temperature", profile.temperature);
    options.generation.repetition_penalty = arguments.float_value(
        "repetition", profile.repetition_penalty);
    options.generation.seed = arguments.u64_value("seed", options.generation.seed);
    return options;
}

[[nodiscard]] std::string required_text(const Arguments& arguments,
                                        std::string_view option_name) {
    std::string text = arguments.get(option_name);
    if (text.empty()) {
        text = arguments.joined_positional();
    }
    if (text.empty()) {
        throw Error("informe --" + std::string(option_name) + " \"texto\"");
    }
    return text;
}

void print_help() {
    std::cout << R"HELP(Cortex Native AI — motor neural C++20 sem dependências externas

Uso:
  cortex train    --corpus ARQUIVO [--state DIR] [opções]
  cortex generate --prompt "texto" [--state DIR] [opções]
  cortex ask      --prompt "pergunta" [--state DIR] [opções]
  cortex analyze  --text "texto" [--prompt "pergunta original"]
  cortex doctor   [--state DIR] (diagnóstico de execução, inclusive Windows)
  cortex remember --text "fato persistente" [--state DIR]
  cortex recall   --query "consulta" [--state DIR] [--limit 5]
  cortex forget   --id N [--state DIR]
  cortex chat     [--state DIR] [opções]
  cortex serve    [--state DIR] [--host 0.0.0.0] [--port 8080]
  cortex models   (lista modelos abertos e recomendação por RAM)
  cortex inspect  [--state DIR]
  cortex benchmark [--state DIR] [--steps 1000]

Treinamento:
  --merges 160 --min-frequency 2 --embedding 256 --hidden 256 --layers 2
  --epochs 25 --sequence 64 --learning-rate 0.0025 --gradient-clip 1.0

Geração/agente:
  --power flash|medium|high (custo, contexto e revisão)
  --tokens N --top-k N --temperature N --repetition N --seed N
  --memory-hits N --memory-score N --context N --no-remember

Interface web, modelo aberto local, internet e APIs:
  --host 0.0.0.0 --port 8080 --workers 4 [--no-internet]
  --local [--local-profile tiny|edge|daily|workstation|max]
  --billion  (atalho local sem chave para Qwen3 1.7B)
  --local-model ARQUIVO.gguf | --local-hf ORG/MODELO:QUANTIZACAO
  --llama-server CAMINHO --local-port 8081 --local-context 8192
  --local-threads N --local-gpu-layers N --local-startup-timeout 1200
  --gemini  (compatibilidade remota opt-in; nunca ativada automaticamente)
  --llm-url URL --llm-model MODELO [--llm-key-env NOME]
  --llm-reasoning low|medium|high [--llm-timeout 45]
  --enable-self-edit --source-root .   (proposta + confirmação humana obrigatória)
  --unrestricted-self-edit             (arquivos novos/externos; somente loopback)

Exemplo rápido:
  make release
  ./build/cortex train --corpus data/corpus.pt.txt --state state
  ./build/cortex serve --state state --host 0.0.0.0 --port 8080
  # abra http://localhost:8080 e converse no navegador
)HELP";
}

void command_train(const Arguments& arguments) {
    const StatePaths paths = state_paths(arguments);
    const fs::path corpus_path = arguments.get("corpus", "data/corpus.pt.txt");
    const std::string corpus = read_text_file(corpus_path);
    fs::create_directories(paths.directory);

    cortex::BpeTokenizer tokenizer;
    tokenizer.train(corpus,
                    arguments.size_value("merges", 160U),
                    arguments.size_value("min-frequency", 2U));
    std::vector<cortex::TokenId> tokens = tokenizer.encode(corpus, true, true);

    cortex::ModelConfig config;
    config.vocabulary_size = tokenizer.vocabulary_size();
    config.embedding_size = arguments.size_value("embedding", config.embedding_size);
    config.hidden_size = arguments.size_value("hidden", config.hidden_size);
    config.layer_count = arguments.size_value("layers", config.layer_count);
    config.seed = arguments.u64_value("seed", config.seed);
    cortex::NeuralLanguageModel model(config);

    cortex::TrainingOptions options;
    options.epochs = arguments.size_value("epochs", options.epochs);
    options.sequence_length = arguments.size_value("sequence", options.sequence_length);
    options.learning_rate = arguments.float_value("learning-rate", options.learning_rate);
    options.gradient_clip = arguments.float_value("gradient-clip", options.gradient_clip);

    std::cout << "Corpus: " << corpus.size() << " bytes, " << tokens.size() << " tokens\n"
              << "Vocabulário: " << tokenizer.vocabulary_size() << " ("
              << tokenizer.merge_count() << " merges)\n"
              << "Parâmetros treináveis: " << model.parameter_count() << "\n";
    const auto start = std::chrono::steady_clock::now();
    model.train(tokens, options, [](const cortex::TrainingMetrics& metrics) {
        std::cout << "época " << metrics.epoch
                  << " | loss=" << std::fixed << std::setprecision(5)
                  << metrics.average_loss
                  << " | perplexidade=" << metrics.perplexity << '\n';
    });
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    tokenizer.save(paths.tokenizer);
    model.save(paths.model);
    // Embeddings antigos pertencem a outro espaço vetorial; novo treino inicia memória limpa.
    cortex::LongTermMemory memory(config.hidden_size);
    memory.save(paths.memory);
    std::cout << "Estado salvo em " << paths.directory << " após "
              << std::setprecision(3) << seconds << " s.\n";
}

void command_generate(const Arguments& arguments) {
    const StatePaths paths = state_paths(arguments);
    cortex::BpeTokenizer tokenizer = cortex::BpeTokenizer::load(paths.tokenizer);
    cortex::NeuralLanguageModel model = cortex::NeuralLanguageModel::load(paths.model);
    if (tokenizer.vocabulary_size() != model.config().vocabulary_size) {
        throw Error("tokenizador e modelo incompatíveis");
    }
    const std::string prompt = required_text(arguments, "prompt");
    std::vector<cortex::TokenId> encoded = tokenizer.encode(prompt, true, false);
    const std::size_t context_limit = arguments.size_value("context", 512U);
    if (encoded.size() > context_limit && context_limit > 1U) {
        std::vector<cortex::TokenId> tail;
        tail.reserve(context_limit);
        tail.push_back(cortex::BpeTokenizer::bos_id);
        tail.insert(tail.end(), encoded.end() - static_cast<std::ptrdiff_t>(context_limit - 1U),
                    encoded.end());
        encoded = std::move(tail);
    }
    const std::vector<cortex::TokenId> generated = model.generate(
        encoded, generation_options(arguments));
    std::cout << tokenizer.decode(generated) << '\n';
}

void command_ask(const Arguments& arguments) {
    const StatePaths paths = state_paths(arguments);
    Runtime runtime = load_runtime(paths);
    cortex::CognitiveAgent agent(runtime.tokenizer, runtime.model, runtime.memory);
    const std::string prompt = required_text(arguments, "prompt");
    const cortex::AgentReply reply = agent.respond(prompt, agent_options(arguments));
    std::cout << reply.text << '\n';
    if (reply.stored_memory_id != 0U) {
        runtime.memory.save(paths.memory);
    }
    if (arguments.flag("show-memory", false)) {
        std::cerr << "Memórias recuperadas: " << reply.recalled_memories.size() << '\n';
        for (const cortex::MemoryHit& hit : reply.recalled_memories) {
            std::cerr << "  #" << hit.id << " score=" << hit.score << " " << hit.text << '\n';
        }
    }
}

void command_analyze(const Arguments& arguments) {
    const std::string text = required_text(arguments, "text");
    const std::string prompt = arguments.get("prompt");
    std::cout << (prompt.empty()
        ? cortex::format_text_analysis(text)
        : cortex::format_response_analysis(prompt, text)) << '\n';
}

void command_doctor(const Arguments& arguments) {
    const StatePaths paths = state_paths(arguments);
    const std::string llama_name = arguments.get(
        "llama-server", environment_value("CORTEX_LLAMA_SERVER", "llama-server"));
    const std::optional<fs::path> curl = executable_on_path(
#ifdef _WIN32
        "curl.exe"
#else
        "curl"
#endif
    );
    const std::optional<fs::path> llama = executable_on_path(llama_name);
    const std::size_t memory_gib = cortex::system_memory_gib();
    const bool state_ready = fs::is_regular_file(paths.tokenizer)
                             && fs::is_regular_file(paths.model);
    const bool billion_ready = memory_gib >= 4U && llama.has_value();
    std::cout << "Diagnóstico Cortex\n"
#ifdef _WIN32
              << "- Plataforma compilada: Windows nativo\n"
#else
              << "- Plataforma compilada: POSIX (gere cortex.exe no Windows com build-windows.cmd)\n"
#endif
              << "- RAM detectada: " << memory_gib << " GiB\n"
              << "- Estado GRU: " << (state_ready ? "pronto" : "ausente; execute train") << '\n'
              << "- Cliente HTTPS curl: " << (curl ? curl->string() : "não encontrado") << '\n'
              << "- llama-server: " << (llama ? llama->string() : "não encontrado") << '\n'
              << "- Camada >= 1 bilhão: "
              << (billion_ready ? "pronta para Qwen3 1.7B"
                                : "requer pelo menos 4 GiB e llama-server") << '\n'
              << "- Comando recomendado: cortex serve --state state --billion --power medium\n";
}

void command_remember(const Arguments& arguments) {
    const StatePaths paths = state_paths(arguments);
    Runtime runtime = load_runtime(paths);
    cortex::CognitiveAgent agent(runtime.tokenizer, runtime.model, runtime.memory);
    const std::uint64_t id = agent.remember(required_text(arguments, "text"));
    runtime.memory.save(paths.memory);
    std::cout << "Memória #" << id << " persistida.\n";
}

void command_recall(const Arguments& arguments) {
    const StatePaths paths = state_paths(arguments);
    Runtime runtime = load_runtime(paths);
    cortex::CognitiveAgent agent(runtime.tokenizer, runtime.model, runtime.memory);
    const std::vector<cortex::MemoryHit> hits = agent.recall(
        required_text(arguments, "query"),
        arguments.size_value("limit", 5U),
        arguments.float_value("minimum-score", -1.0F));
    for (const cortex::MemoryHit& hit : hits) {
        std::cout << '#' << hit.id << " score=" << std::fixed << std::setprecision(5)
                  << hit.score << "\n" << hit.text << "\n\n";
    }
    if (hits.empty()) {
        std::cout << "Nenhuma memória compatível.\n";
    }
}

void command_forget(const Arguments& arguments) {
    if (!arguments.has("id")) {
        throw Error("informe --id N");
    }
    const StatePaths paths = state_paths(arguments);
    Runtime runtime = load_runtime(paths);
    const std::uint64_t id = arguments.u64_value("id", 0U);
    if (!runtime.memory.erase(id)) {
        throw Error("id de memória não encontrado");
    }
    runtime.memory.save(paths.memory);
    std::cout << "Memória #" << id << " removida.\n";
}

void command_chat(const Arguments& arguments) {
    const StatePaths paths = state_paths(arguments);
    Runtime runtime = load_runtime(paths);
    cortex::CognitiveAgent agent(runtime.tokenizer, runtime.model, runtime.memory);
    cortex::AgentOptions options = agent_options(arguments);
    const std::uint64_t initial_seed = options.generation.seed;
    std::size_t turn = 0U;

    std::cout << "Chat nativo iniciado. Comandos: :sair, :lembrar TEXTO, :recordar TEXTO\n";
    std::string line;
    while (std::cout << "você> " && std::getline(std::cin, line)) {
        if (line == ":sair") {
            break;
        }
        if (line.rfind(":lembrar ", 0U) == 0U) {
            const std::uint64_t id = agent.remember(line.substr(10U));
            runtime.memory.save(paths.memory);
            std::cout << "memória> #" << id << " salva\n";
            continue;
        }
        if (line.rfind(":recordar ", 0U) == 0U) {
            for (const cortex::MemoryHit& hit : agent.recall(line.substr(10U))) {
                std::cout << "memória> #" << hit.id << " (" << hit.score << ") "
                          << hit.text << '\n';
            }
            continue;
        }
        if (line.empty()) {
            continue;
        }

        options.generation.seed = initial_seed + static_cast<std::uint64_t>(turn++);
        const cortex::AgentReply reply = agent.respond(line, options);
        std::cout << "cortex> " << reply.text << '\n';
        if (reply.stored_memory_id != 0U) {
            runtime.memory.save(paths.memory);
        }
    }
}

void command_serve(const Arguments& arguments) {
    const StatePaths paths = state_paths(arguments);
    cortex::WebServerOptions options;
    options.bind_address = arguments.get("host", options.bind_address);
    options.unrestricted_self_edit = arguments.flag("unrestricted-self-edit", false);
    options.self_edit_enabled = arguments.flag("enable-self-edit", false)
                                || options.unrestricted_self_edit;
    if (options.self_edit_enabled && options.bind_address != "127.0.0.1") {
        throw Error("autoedição exige --host 127.0.0.1 para pedir autorização local");
    }
    options.source_root = arguments.get("source-root", ".");
    Runtime runtime = load_runtime(paths);
    const std::size_t port = arguments.size_value("port", options.port);
    if (port == 0U || port > 65'535U) {
        throw Error("--port deve estar entre 1 e 65535");
    }
    options.port = static_cast<std::uint16_t>(port);
    options.worker_count = arguments.size_value("workers", options.worker_count);
    options.memory_path = paths.memory;
    options.agent = agent_options(arguments);
    options.internet_enabled = !arguments.flag("no-internet", false);
    options.language_api.endpoint = arguments.get(
        "llm-url", environment_value("CORTEX_LLM_URL"));
    options.language_api.model = arguments.get(
        "llm-model", environment_value("CORTEX_LLM_MODEL"));

    const bool custom_local_path = arguments.has("local-model");
    const bool custom_local_hf = arguments.has("local-hf");
    const bool billion_requested = arguments.flag("billion", false);
    if (custom_local_path && custom_local_hf) {
        throw Error("use somente um entre --local-model e --local-hf");
    }
    if (arguments.has("local-profile") && (custom_local_path || custom_local_hf)) {
        throw Error("--local-profile não pode ser combinado com uma fonte local customizada");
    }
    if (billion_requested && (custom_local_path || custom_local_hf
                              || (arguments.has("local-profile")
                                  && arguments.get("local-profile") != "edge"))) {
        throw Error("--billion usa o perfil edge 1.7B e não aceita outro modelo");
    }
    const bool local_requested = arguments.flag("local", false) || billion_requested
                                 || arguments.has("local-profile")
                                 || custom_local_path || custom_local_hf;
    std::optional<cortex::OpenModelProfile> selected_profile;
    std::optional<cortex::LocalModelServer> local_server;

    if (local_requested) {
        cortex::LocalModelOptions local;
        if (!custom_local_path && !custom_local_hf) {
            const std::size_t memory_gib = cortex::system_memory_gib();
            selected_profile = billion_requested
                ? cortex::open_model_profile("edge")
                : arguments.has("local-profile")
                    ? cortex::open_model_profile(arguments.get("local-profile"))
                    : cortex::recommended_open_model_profile(memory_gib);
            std::cout << "Perfil local: " << selected_profile->id << " ("
                      << selected_profile->display_name << ") | RAM detectada: "
                      << (memory_gib == 0U ? "desconhecida" : std::to_string(memory_gib) + " GiB")
                      << '\n';
            if (memory_gib != 0U && memory_gib < selected_profile->minimum_ram_gib) {
                std::cerr << "aviso: o perfil recomenda pelo menos "
                          << selected_profile->minimum_ram_gib << " GiB de RAM\n";
            }
            local.hugging_face_reference = selected_profile->hugging_face_reference;
            local.context_tokens = selected_profile->recommended_context;
        } else if (custom_local_path) {
            local.model_path = arguments.get("local-model");
        } else {
            local.hugging_face_reference = arguments.get("local-hf");
        }
        local.executable = arguments.get(
            "llama-server", environment_value("CORTEX_LLAMA_SERVER", "llama-server"));
        local.alias = arguments.get("local-alias", "cortex-local");
        const std::size_t local_port = arguments.size_value("local-port", local.port);
        if (local_port == 0U || local_port > 65'535U) {
            throw Error("--local-port deve estar entre 1 e 65535");
        }
        local.port = static_cast<std::uint16_t>(local_port);
        local.context_tokens = arguments.size_value("local-context", local.context_tokens);
        local.threads = arguments.size_value("local-threads", local.threads);
        local.gpu_layers = arguments.int_value("local-gpu-layers", local.gpu_layers);
        local.startup_timeout_seconds = arguments.size_value(
            "local-startup-timeout", local.startup_timeout_seconds);

        local_server.emplace(local);
        local_server->start();
        options.language_api.endpoint = local_server->endpoint();
        options.language_api.model = local.alias;
        options.language_api.provider = "local-open-model";
        options.language_api.display_model = selected_profile
            ? selected_profile->display_name
            : arguments.get("local-name",
                            custom_local_path ? local.model_path.filename().string()
                                              : local.hugging_face_reference);
        options.language_api.parameter_count = selected_profile
                                               ? selected_profile->parameter_count : 0U;
        options.language_api.context_tokens = local.context_tokens;
        options.language_api.api_key_environment.clear();
        options.language_api.reasoning_effort.clear();
        options.language_api.extended_sampling = true;
        options.language_api.top_p = 0.8F;
        options.language_api.presence_penalty = 1.5F;
    } else {
        // Provedores remotos são compatibilidade explicitamente opt-in; o padrão sempre é local.
        const bool gemini_flag = arguments.flag("gemini", false);
        const bool gemini = gemini_flag;
        if (gemini && (options.language_api.endpoint.empty()
                       || (gemini_flag && !arguments.has("llm-url")))) {
            options.language_api.endpoint =
                "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions";
        }
        if (gemini && !arguments.has("llm-model")) {
            options.language_api.model = environment_value("GEMINI_MODEL", "gemini-3.7-flash");
        }
        options.language_api.provider = gemini ? "gemini" : "openai-compatible";
        options.language_api.api_key_environment = arguments.get(
            "llm-key-env", gemini ? "GEMINI_API_KEY" : "CORTEX_LLM_API_KEY");
        options.language_api.reasoning_effort = arguments.get(
            "llm-reasoning", gemini
                ? environment_value("GEMINI_REASONING_EFFORT", "medium")
                : environment_value("CORTEX_LLM_REASONING"));
    }
    options.language_api.timeout_seconds = arguments.size_value(
        "llm-timeout", local_requested ? 180U : 45U);
    if (options.language_api.timeout_seconds == 0U
        || options.language_api.timeout_seconds > 300U) {
        throw Error("--llm-timeout deve estar entre 1 e 300 segundos");
    }
    cortex::run_web_chat_server(runtime.tokenizer, runtime.model, runtime.memory, options);
}

void command_models() {
    const std::size_t memory_gib = cortex::system_memory_gib();
    const cortex::OpenModelProfile recommended = cortex::recommended_open_model_profile(memory_gib);
    std::cout << "RAM detectada: "
              << (memory_gib == 0U ? "desconhecida" : std::to_string(memory_gib) + " GiB")
              << "\nPerfis GGUF disponíveis:\n";
    for (const cortex::OpenModelProfile& profile : cortex::available_open_model_profiles()) {
        std::cout << (profile.id == recommended.id ? "* " : "  ")
                  << std::left << std::setw(12) << profile.id
                  << std::setw(27) << profile.display_name
                  << " RAM >= " << profile.minimum_ram_gib << " GiB | ctx "
                  << profile.recommended_context << '\n';
    }
    std::cout << "* recomendado automaticamente; inicie com: cortex serve --state state --local\n";
}

void command_inspect(const Arguments& arguments) {
    const StatePaths paths = state_paths(arguments);
    Runtime runtime = load_runtime(paths);
    const cortex::ModelConfig& config = runtime.model.config();
    std::cout << "Estado: " << paths.directory << '\n'
              << "Vocabulário: " << config.vocabulary_size << " ("
              << runtime.tokenizer.merge_count() << " merges)\n"
              << "Embedding: " << config.embedding_size << '\n'
              << "Oculto: " << config.hidden_size << '\n'
              << "Camadas GRU: " << config.layer_count << '\n'
              << "Parâmetros: " << runtime.model.parameter_count() << '\n'
              << "Passos Adam: " << runtime.model.optimizer_step() << '\n'
              << "Memórias: " << runtime.memory.size() << '\n'
              << "Threads auxiliares: " << cortex::global_thread_pool().worker_count() << '\n';
}

void command_benchmark(const Arguments& arguments) {
    const StatePaths paths = state_paths(arguments);
    cortex::BpeTokenizer tokenizer = cortex::BpeTokenizer::load(paths.tokenizer);
    cortex::NeuralLanguageModel model = cortex::NeuralLanguageModel::load(paths.model);
    const std::size_t steps = arguments.size_value("steps", 1'000U);
    if (steps == 0U) {
        throw Error("--steps deve ser positivo");
    }
    const std::vector<cortex::TokenId> seed_tokens = tokenizer.encode(
        arguments.get("prompt", "benchmark nativo"), true, false);
    cortex::NeuralLanguageModel::State state = model.zero_state();
    std::vector<float> logits;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t index = 0U; index < steps; ++index) {
        const cortex::TokenId token = seed_tokens[index % seed_tokens.size()];
        logits = model.step(token, state);
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    // O uso impede o compilador de eliminar o loop inteiro no release/LTO.
    volatile float guard = logits.empty() ? 0.0F : logits.front();
    static_cast<void>(guard);
    std::cout << steps << " passos em " << seconds << " s = "
              << static_cast<double>(steps) / seconds << " tokens/s\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            print_help();
            return EXIT_SUCCESS;
        }
        const std::string command = argv[1];
        const Arguments arguments(argc, argv, 2);
        if (command == "help" || command == "--help" || command == "-h") {
            print_help();
        } else if (command == "train") {
            command_train(arguments);
        } else if (command == "generate") {
            command_generate(arguments);
        } else if (command == "ask") {
            command_ask(arguments);
        } else if (command == "analyze") {
            command_analyze(arguments);
        } else if (command == "doctor") {
            command_doctor(arguments);
        } else if (command == "remember") {
            command_remember(arguments);
        } else if (command == "recall") {
            command_recall(arguments);
        } else if (command == "forget") {
            command_forget(arguments);
        } else if (command == "chat") {
            command_chat(arguments);
        } else if (command == "serve") {
            command_serve(arguments);
        } else if (command == "models") {
            command_models();
        } else if (command == "inspect") {
            command_inspect(arguments);
        } else if (command == "benchmark") {
            command_benchmark(arguments);
        } else {
            throw Error("comando desconhecido: " + command);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "erro: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
