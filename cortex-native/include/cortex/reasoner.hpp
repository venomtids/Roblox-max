#pragma once

#include "cortex/memory.hpp"
#include "cortex/neural_model.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cortex {

// Identifica como uma resposta local foi produzida. A UI usa este valor para não
// apresentar uma regra, uma ferramenta ou uma recusa segura como texto gerado pelo GRU.
enum class LocalResponseMode : std::uint8_t {
    neural,
    knowledge,
    calculation,
    benchmark,
    memory,
    safe_fallback
};

struct NativeAnswer final {
    std::string text;
    LocalResponseMode mode = LocalResponseMode::knowledge;
};

// Resolve intenções verificáveis antes da geração probabilística: cálculos,
// benchmark real, memória e conhecimento técnico da própria Cortex.
[[nodiscard]] std::optional<NativeAnswer> reason_locally(
    std::string_view prompt,
    std::string_view conversation_context,
    const NeuralLanguageModel& model,
    std::span<const MemoryHit> memories = {});

// Impede que uma continuação fluente, porém desconectada da pergunta, chegue à UI.
[[nodiscard]] bool neural_response_is_relevant(std::string_view prompt,
                                               std::string_view response);

// Resposta explícita usada quando o pequeno GRU não possui evidência confiável.
[[nodiscard]] NativeAnswer safe_local_fallback();

// Nome estável enviado como provider no protocolo HTTP.
[[nodiscard]] std::string_view local_response_mode_name(LocalResponseMode mode) noexcept;

} // namespace cortex
