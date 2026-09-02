#pragma once

#include "cortex/memory.hpp"
#include "cortex/neural_model.hpp"
#include "cortex/quality.hpp"
#include "cortex/reasoner.hpp"
#include "cortex/tokenizer.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cortex {

struct AgentOptions final {
    PowerMode power_mode = PowerMode::medium;
    std::size_t memory_hits = 6U;
    float minimum_memory_score = 0.65F;
    std::size_t maximum_context_tokens = 2'048U;
    bool remember_interaction = true;
    GenerationOptions generation;
};

struct AgentReply final {
    std::string text;
    std::vector<TokenId> generated_tokens;
    std::vector<MemoryHit> recalled_memories;
    std::uint64_t stored_memory_id = 0U;
    LocalResponseMode response_mode = LocalResponseMode::neural;
};

// Recorta continuações de corpus para devolver somente o turno atual do assistente.
[[nodiscard]] std::string extract_direct_answer(std::string_view generated_text);

// Orquestra recuperação, construção de contexto, inferência e consolidação episódica.
class CognitiveAgent final {
public:
    CognitiveAgent(const BpeTokenizer& tokenizer,
                   const NeuralLanguageModel& model,
                   LongTermMemory& memory);

    [[nodiscard]] std::uint64_t remember(std::string text);
    [[nodiscard]] std::vector<MemoryHit> recall(std::string_view query,
                                                std::size_t limit = 5U,
                                                float minimum_score = -1.0F) const;
    [[nodiscard]] AgentReply respond(std::string_view prompt,
                                     const AgentOptions& options = {},
                                     std::string_view conversation_context = {});

private:
    [[nodiscard]] std::vector<float> embed_text(std::string_view text) const;
    [[nodiscard]] std::vector<TokenId> bounded_context(
        std::string_view prompt,
        std::string_view conversation_context,
        std::span<const MemoryHit> hits,
        std::size_t maximum_tokens) const;

    const BpeTokenizer& tokenizer_;
    const NeuralLanguageModel& model_;
    LongTermMemory& memory_;
};

} // namespace cortex
