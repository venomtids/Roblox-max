#pragma once

#include "cortex/core.hpp"

#include <filesystem>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cortex {

struct MemoryHit final {
    std::uint64_t id = 0U;
    std::uint64_t created_unix_ms = 0U;
    float score = 0.0F;
    std::string text;
};

// Vector store persistente e thread-safe para memória episódica de longo prazo.
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
    std::uint64_t remember(std::string text, std::span<const float> embedding);
    bool erase(std::uint64_t id);

    // Busca cosseno exata e paralela; adequada como baseline correto e determinístico.
    [[nodiscard]] std::vector<MemoryHit> search(std::span<const float> query,
                                                std::size_t limit,
                                                float minimum_score = -1.0F) const;

    void save(const std::filesystem::path& path) const;
    [[nodiscard]] static LongTermMemory load(const std::filesystem::path& path,
                                              std::size_t expected_dimension = 0U);

private:
    struct Record final {
        std::uint64_t id = 0U;
        std::uint64_t created_unix_ms = 0U;
        std::string text;
        std::vector<float> embedding;
    };

    static std::uint64_t current_unix_ms();
    void validate_embedding(std::span<const float> embedding) const;

    std::size_t dimension_ = 0U;
    std::uint64_t next_id_ = 1U;
    std::vector<Record> records_;
    mutable std::shared_mutex mutex_;
};

} // namespace cortex
