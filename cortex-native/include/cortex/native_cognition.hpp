#pragma once

#include "cortex/knowledge.hpp"
#include "cortex/memory.hpp"
#include "cortex/quality.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cortex {

// Intenções explícitas tornam o caminho nativo auditável. O motor não finge que uma
// regra é um LLM: ele informa qual especialista determinístico construiu a resposta.
enum class NativeCognitiveIntent : std::uint8_t {
    conversation,
    explanation,
    comparison,
    procedure,
    decision,
    reasoning,
    brainstorming,
    creative_writing,
    long_form_writing,
    summarization,
    programming
};

struct NativeCognitionOptions final {
    PowerMode power_mode = PowerMode::medium;
    std::size_t maximum_evidence = 4U;
    std::size_t maximum_output_bytes = 24U * 1024U;
};

struct NativeCognitionResult final {
    std::string text;
    NativeCognitiveIntent intent = NativeCognitiveIntent::explanation;
    double confidence = 0.0;
    std::size_t evidence_count = 0U;
    bool used_memory = false;
    bool generated_code = false;
};

// Planejador/sintetizador neuro-simbólico para o modo sem GGUF. Combina especialistas
// de escrita, código, resumo, decisão e conhecimento recuperado; ausência de evidência
// factual devolve nullopt para que o chamador use uma recusa honesta.
[[nodiscard]] std::optional<NativeCognitionResult> synthesize_native_response(
    std::string_view prompt,
    std::string_view conversation_context,
    std::span<const MemoryHit> memories = {},
    const NativeCognitionOptions& options = {},
    std::span<const CuratedKnowledgeAnswer> supplied_evidence = {});

[[nodiscard]] std::string_view native_cognitive_intent_name(
    NativeCognitiveIntent intent) noexcept;

// Embedding esparso-hash independente do corpus neural. Palavras, radicais, bigramas e
// trigramas de caracteres melhoram a recuperação em português mesmo antes do GRU treinar.
[[nodiscard]] std::vector<float> native_feature_embedding(std::string_view text,
                                                           std::size_t dimension);

// Ingestão incremental: quebra um documento em unidades persistíveis sem cortar UTF-8.
// Cada trecho conserva a origem para respostas fundamentadas e auditáveis.
[[nodiscard]] std::vector<std::string> chunk_knowledge_document(
    std::string_view source,
    std::string_view text,
    std::size_t maximum_chunk_bytes = 2'048U);

} // namespace cortex
