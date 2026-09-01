#pragma once

#include "cortex/memory.hpp"
#include "cortex/neural_model.hpp"
#include "cortex/tokenizer.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cortex {

struct AgentOptions final {
    std::size_t memory_hits = 3U;
    float minimum_memory_score = 0.10F;
    std::size_t maximum_context_tokens = 512U;
    bool remember_interaction = true;
    GenerationOptions generation;
};

struct AgentReply final {
    std::string text;
    std::vector<TokenId> generated_tokens;
    std::vector<MemoryHit> recalled_memories;
    std::uint64_t stored_memory_id = 0U;
};

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
                                     const AgentOptions& options = {});

private:
    [[nodiscard]] std::vector<float> embed_text(std::string_view text) const;
    [[nodiscard]] std::vector<TokenId> bounded_context(std::string_view prompt,
                                                        std::span<const MemoryHit> hits,
                                                        std::size_t maximum_tokens) const;

    const BpeTokenizer& tokenizer_;
    const NeuralLanguageModel& model_;
    LongTermMemory& memory_;
};

} // namespace cortex
