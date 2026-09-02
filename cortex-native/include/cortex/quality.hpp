#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cortex {

// Perfis de custo previsíveis. Eles não fingem trocar os pesos carregados: controlam
// contexto, recuperação, comprimento, amostragem e a segunda passagem de revisão.
enum class PowerMode : std::uint8_t {
    flash,
    medium,
    high
};

struct PowerProfile final {
    PowerMode mode = PowerMode::medium;
    std::string_view name;
    std::string_view display_name;
    std::string_view response_directive;
    std::size_t maximum_tokens = 1'024U;
    std::size_t memory_hits = 6U;
    std::size_t context_tokens = 2'048U;
    std::size_t top_k = 20U;
    float temperature = 0.5F;
    float repetition_penalty = 1.10F;
    bool revision_pass = false;
};

[[nodiscard]] PowerMode parse_power_mode(std::string_view value);
[[nodiscard]] std::string_view power_mode_name(PowerMode mode) noexcept;
[[nodiscard]] const PowerProfile& power_profile(PowerMode mode) noexcept;

// Métricas calculadas diretamente sobre UTF-8, sem bibliotecas linguísticas opacas.
struct TextMetrics final {
    std::size_t bytes = 0U;
    std::size_t characters = 0U;
    std::size_t words = 0U;
    std::size_t unique_words = 0U;
    std::size_t sentences = 0U;
    std::size_t paragraphs = 0U;
    std::size_t code_blocks = 0U;
    std::size_t longest_sentence_words = 0U;
    double average_words_per_sentence = 0.0;
    double lexical_diversity = 0.0;
    double repeated_word_ratio = 0.0;
    double long_sentence_ratio = 0.0;
    double readability_score = 0.0;           // Facilidade estrutural, não correção factual.
    double structural_coherence_score = 0.0;  // Ritmo/repetição, não semântica profunda.
    std::vector<std::string> top_terms;
};

struct ResponseAssessment final {
    bool accepted = false;
    double quality_score = 0.0;
    double relevance_score = 0.0;
    double coherence_score = 0.0;
    TextMetrics metrics;
    std::vector<std::string> issues;
};

// Retorna termos normalizados, únicos e sem palavras funcionais comuns. Essa mesma
// representação fundamenta o filtro de resposta e a busca na base curada.
[[nodiscard]] std::vector<std::string> significant_words(std::string_view text);
[[nodiscard]] TextMetrics analyze_text(std::string_view text);
[[nodiscard]] ResponseAssessment assess_response(std::string_view prompt,
                                                  std::string_view response);
[[nodiscard]] std::string format_text_analysis(std::string_view text);
[[nodiscard]] std::string format_response_analysis(std::string_view prompt,
                                                    std::string_view response);

struct BehaviorSnapshot final {
    std::uint64_t response_count = 0U;
    std::uint64_t rejected_count = 0U;
    std::uint64_t revised_count = 0U;
    double average_quality = 0.0;
};

// Telemetria agregada em memória: nenhum conteúdo do usuário é copiado para o monitor.
class BehaviorMonitor final {
public:
    void record(const ResponseAssessment& assessment, bool revised) noexcept;
    [[nodiscard]] BehaviorSnapshot snapshot() const noexcept;

private:
    std::atomic<std::uint64_t> response_count_{0U};
    std::atomic<std::uint64_t> rejected_count_{0U};
    std::atomic<std::uint64_t> revised_count_{0U};
    std::atomic<std::uint64_t> quality_micros_{0U};
};

} // namespace cortex
