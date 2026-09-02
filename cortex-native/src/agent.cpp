#include "cortex/agent.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>
#include <utility>

namespace cortex {
namespace {

[[nodiscard]] bool response_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r'
           || character == '\n' || character == '\f' || character == '\v';
}

} // namespace

std::string extract_direct_answer(std::string_view generated_text) {
    // Remove espaços que o modelo pode emitir antes do primeiro conteúdo útil.
    while (!generated_text.empty() && response_space(generated_text.front())) {
        generated_text.remove_prefix(1U);
    }

    // Alguns checkpoints repetem o papel antes da resposta; ele não deve aparecer na UI.
    constexpr std::array<std::string_view, 3U> leading_roles{
        "Assistente:", "Assistant:", "Cortex:"
    };
    for (const std::string_view role : leading_roles) {
        if (generated_text.starts_with(role)) {
            generated_text.remove_prefix(role.size());
            while (!generated_text.empty() && response_space(generated_text.front())) {
                generated_text.remove_prefix(1U);
            }
            break;
        }
    }

    // O corpus usa linha em branco e marcadores de papel entre turnos. A geração neural
    // pode continuar prevendo o diálogo seguinte; devolvemos apenas o primeiro turno.
    constexpr std::array<std::string_view, 10U> stop_markers{
        "\r\n\r\n", "\n\n", "\nUsuário:", "\nUsuario:", "\nUser:",
        "\nSistema:", "\nSystem:", "\nAssistente:", "\nAssistant:", "\nCortex:"
    };
    std::size_t end = generated_text.size();
    for (const std::string_view marker : stop_markers) {
        const std::size_t found = generated_text.find(marker);
        if (found != std::string_view::npos) {
            end = std::min(end, found);
        }
    }
    generated_text = generated_text.substr(0U, end);
    while (!generated_text.empty() && response_space(generated_text.back())) {
        generated_text.remove_suffix(1U);
    }
    return std::string(generated_text);
}

CognitiveAgent::CognitiveAgent(const BpeTokenizer& tokenizer,
                               const NeuralLanguageModel& model,
                               LongTermMemory& memory)
    : tokenizer_(tokenizer), model_(model), memory_(memory) {
    if (tokenizer_.vocabulary_size() != model_.config().vocabulary_size) {
        throw Error("tokenizador e modelo possuem vocabulários diferentes");
    }
    if (memory_.dimension() != model_.config().hidden_size) {
        throw Error("memória e modelo possuem dimensões semânticas diferentes");
    }
}

std::vector<float> CognitiveAgent::embed_text(std::string_view text) const {
    std::vector<TokenId> tokens = tokenizer_.encode(text, true, false);
    // Embeddings episódicos não precisam percorrer documentos inteiros. Limitar o
    // GRU auxiliar preserva começo/fim e impede que RAG bloqueie um LLM grande.
    constexpr std::size_t maximum_embedding_tokens = 256U;
    if (tokens.size() > maximum_embedding_tokens) {
        constexpr std::size_t head_tokens = maximum_embedding_tokens / 2U;
        std::vector<TokenId> bounded;
        bounded.reserve(maximum_embedding_tokens);
        bounded.insert(bounded.end(), tokens.begin(),
                       tokens.begin() + static_cast<std::ptrdiff_t>(head_tokens));
        bounded.insert(bounded.end(),
                       tokens.end() - static_cast<std::ptrdiff_t>(maximum_embedding_tokens
                                                                  - head_tokens),
                       tokens.end());
        tokens = std::move(bounded);
    }
    return model_.sentence_embedding(tokens);
}

std::uint64_t CognitiveAgent::remember(std::string text) {
    const std::vector<float> embedding = embed_text(text);
    return memory_.remember(std::move(text), embedding);
}

std::vector<MemoryHit> CognitiveAgent::recall(std::string_view query,
                                              std::size_t limit,
                                              float minimum_score) const {
    const std::vector<float> embedding = embed_text(query);
    return memory_.search(embedding, limit, minimum_score);
}

std::vector<TokenId> CognitiveAgent::bounded_context(
    std::string_view prompt,
    std::string_view conversation_context,
    std::span<const MemoryHit> hits,
    std::size_t maximum_tokens) const {
    if (maximum_tokens < 8U) {
        throw Error("maximum_context_tokens deve ser pelo menos 8");
    }

    std::ostringstream context;
    // A frase coincide com o formato do corpus demonstrativo e estabiliza o papel do modelo.
    context << "Sistema: Você é Cortex, um assistente técnico nativo, objetivo, seguro e "
               "verificável.\n";
    if (!hits.empty()) {
        context << "Memórias relevantes:\n";
        for (const MemoryHit& hit : hits) {
            // Limitar cada episódio evita que uma única memória domine todo o contexto.
            constexpr std::size_t maximum_excerpt = 1'024U;
            const std::string excerpt = hit.text.substr(0U, maximum_excerpt);
            context << "- " << excerpt << '\n';
        }
    }
    if (!conversation_context.empty()) {
        // O cliente envia somente as mensagens recentes; o limite final continua em tokens.
        constexpr std::size_t maximum_recent_bytes = 16U * 1024U;
        context << "Conversa recente:\n"
                << conversation_context.substr(0U, maximum_recent_bytes) << '\n';
    }
    context << "Usuário: " << prompt << "\nAssistente: ";

    std::vector<TokenId> tokens = tokenizer_.encode(context.str(), true, false);
    if (tokens.size() <= maximum_tokens) {
        return tokens;
    }

    // Preserva BOS e a cauda, onde estão a pergunta atual e o marcador Assistente.
    std::vector<TokenId> bounded;
    bounded.reserve(maximum_tokens);
    bounded.push_back(BpeTokenizer::bos_id);
    const std::size_t tail = maximum_tokens - 1U;
    bounded.insert(bounded.end(), tokens.end() - static_cast<std::ptrdiff_t>(tail),
                   tokens.end());
    return bounded;
}

AgentReply CognitiveAgent::respond(std::string_view prompt,
                                   const AgentOptions& options,
                                   std::string_view conversation_context) {
    if (prompt.empty()) {
        throw Error("o prompt do agente não pode estar vazio");
    }

    std::vector<MemoryHit> hits;
    std::vector<TokenId> generated;
    std::string answer;
    LocalResponseMode response_mode = LocalResponseMode::neural;

    // Intenções verificáveis são resolvidas antes da amostragem. Isso impede que uma palavra
    // como "benchmark" seja confundida com uma saudação aprendida no pequeno corpus.
    std::optional<NativeAnswer> native = reason_locally(
        prompt, conversation_context, model_);
    if (native && native->mode == LocalResponseMode::memory) {
        // Somente uma pergunta sobre lembranças paga o custo da busca antes de uma regra nativa.
        hits = recall(prompt, options.memory_hits, options.minimum_memory_score);
        native = reason_locally(prompt, conversation_context, model_, hits);
    }
    if (native) {
        answer = std::move(native->text);
        response_mode = native->mode;
    } else {
        // Recuperação e inferência ficam desacopladas: memória pode crescer sem retreinar pesos.
        hits = recall(prompt, options.memory_hits, options.minimum_memory_score);
        const std::vector<TokenId> context = bounded_context(
            prompt, conversation_context, hits, options.maximum_context_tokens);
        generated = model_.generate(context, options.generation);
        std::string candidate = extract_direct_answer(tokenizer_.decode(generated));
        if (neural_response_is_relevant(prompt, candidate)) {
            answer = std::move(candidate);
        } else {
            NativeAnswer safe = safe_local_fallback();
            answer = std::move(safe.text);
            response_mode = safe.mode;
            generated.clear(); // a continuação rejeitada não conta como resposta entregue.
        }
    }

    std::uint64_t stored_id = 0U;
    if (options.remember_interaction) {
        std::string episode = "Usuário: ";
        episode.append(prompt);
        episode += "\nAssistente: ";
        episode += answer;
        stored_id = remember(std::move(episode));
    }

    return AgentReply{
        std::move(answer),
        std::move(generated),
        hits,
        stored_id,
        response_mode
    };
}

} // namespace cortex
