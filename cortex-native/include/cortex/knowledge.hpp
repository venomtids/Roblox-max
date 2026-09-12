#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cortex {

struct CuratedKnowledgeAnswer final {
    std::string title;
    std::string category;
    std::string text;
    double score = 0.0;
};

// Base embutida e auditável para fatos estáveis. Ela não tenta responder atualidades:
// para isso a camada de pesquisa HTTPS continua sendo o caminho correto.
// A busca múltipla alimenta o sintetizador cognitivo sem esconder de onde veio cada trecho.
[[nodiscard]] std::vector<CuratedKnowledgeAnswer> search_curated_knowledge(
    std::string_view query,
    std::size_t limit = 4U,
    double minimum_score = 0.12);
[[nodiscard]] std::optional<CuratedKnowledgeAnswer> lookup_curated_knowledge(
    std::string_view query);
[[nodiscard]] std::size_t curated_knowledge_size() noexcept;

} // namespace cortex
