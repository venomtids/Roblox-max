#pragma once

#include "cortex/core.hpp"

#include <filesystem>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cortex {

struct MemoryHit final {
    std::uint64_t id = 0U;
    std::uint64_t created_unix_ms = 0U;
    float score = 0.0F;
    std::string text;
};

// Vector store persistente e thread-safe para memória episódica de longo prazo.
// Combina busca exata determinística com índice ANN (IVF) e deduplicação em escala.
class LongTermMemory final {
public:
    explicit LongTermMemory(std::size_t embedding_dimension);
    LongTermMemory(LongTermMemory&& other) noexcept;
    LongTermMemory& operator=(LongTermMemory&& other) noexcept;

    LongTermMemory(const LongTermMemory&) = delete;
    LongTermMemory& operator=(const LongTermMemory&) = delete;

    [[nodiscard]] std::size_t dimension() const noexcept { return dimension_; }
    [[nodiscard]] std::size_t size() const;

    // A cópia normalizada do embedding é armazenada; o id cresce monotonicamente.
    // Deduplicação: conteúdo idêntico retorna id existente; conteúdo semanticamente
    // quase idêntico (cosine >0.995 + lexical >0.90) também é considerado duplicata em escala.
    std::uint64_t remember(std::string text, std::span<const float> embedding);
    bool erase(std::uint64_t id);

    // Busca cosseno exata e paralela; adequada como baseline correto e determinístico.
    // Para N >= 2048 usa índice ANN IVF (aproximado) com re-ranking exato nos candidatos.
    [[nodiscard]] std::vector<MemoryHit> search(std::span<const float> query,
                                                std::size_t limit,
                                                float minimum_score = -1.0F) const;

    // Combina cosseno com sobreposição lexical. O caminho híbrido reduz falsos negativos
    // do pequeno GRU em nomes, código e português sem exigir outro modelo de embedding.
    [[nodiscard]] std::vector<MemoryHit> hybrid_search(
        std::string_view query_text,
        std::span<const float> query_embedding,
        std::size_t limit,
        float minimum_score = -1.0F) const;

    void save(const std::filesystem::path& path) const;
    [[nodiscard]] static LongTermMemory load(const std::filesystem::path& path,
                                              std::size_t expected_dimension = 0U);

    // --- Extensões para escala ---
    // Força reconstrução do índice ANN (útil para benchmark/teste)
    void rebuild_index();
    [[nodiscard]] bool ann_enabled() const noexcept;
    [[nodiscard]] std::size_t ann_cluster_count() const noexcept;
    [[nodiscard]] std::size_t ann_candidate_count(std::span<const float> query) const;

private:
    struct Record final {
        std::uint64_t id = 0U;
        std::uint64_t created_unix_ms = 0U;
        std::string text;
        std::vector<float> embedding;
        std::vector<std::string> lexical_terms; // índice derivado; não precisa ser serializado.
    };

    static std::uint64_t current_unix_ms();
    void validate_embedding(std::span<const float> embedding) const;

    // --- ANN (IVF) ---
    static constexpr std::size_t ann_threshold_ = 2048U;
    static constexpr std::size_t ann_max_clusters_ = 512U;
    static constexpr std::size_t ann_min_clusters_ = 16U;
    static std::size_t ann_target_clusters(std::size_t record_count) noexcept;
    void mark_ann_dirty_locked() noexcept;
    void rebuild_ann_locked() const; // exige unique_lock
    void ensure_ann_built() const; // faz upgrade de shared->unique se necessário
    [[nodiscard]] std::vector<std::size_t> ann_candidates(
        std::span<const float> normalized_query) const;

    std::size_t dimension_ = 0U;
    std::uint64_t next_id_ = 1U;
    std::vector<Record> records_;
    // Índice derivado e não serializado: hash aponta para posição; colisões confirmam o texto.
    mutable std::unordered_multimap<std::uint64_t, std::size_t> content_index_;
    // Índice lexical invertido para híbrido em escala (termo -> índices)
    mutable std::unordered_map<std::string, std::vector<std::size_t>> lexical_inverted_index_;
    // ANN IVF
    mutable std::vector<std::vector<float>> ann_centroids_;
    mutable std::vector<std::vector<std::size_t>> ann_postings_;
    mutable std::size_t ann_built_upon_ = 0U;
    mutable bool ann_dirty_ = false;
    mutable std::shared_mutex mutex_;
};

} // namespace cortex
