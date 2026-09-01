#include "cortex/agent.hpp"

#include <algorithm>
#include <sstream>

namespace cortex {

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
    context << "Sistema: responda usando padrões aprendidos e memórias relevantes.\n";
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

    // Recuperação e inferência ficam desacopladas: memória pode crescer sem retreinar pesos.
    const std::vector<MemoryHit> hits = recall(prompt, options.memory_hits,
                                               options.minimum_memory_score);
    const std::vector<TokenId> context = bounded_context(
        prompt, conversation_context, hits, options.maximum_context_tokens);
    std::vector<TokenId> generated = model_.generate(context, options.generation);
    std::string answer = tokenizer_.decode(generated);

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
        stored_id
    };
}

} // namespace cortex
