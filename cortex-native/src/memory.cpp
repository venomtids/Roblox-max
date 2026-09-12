#include "cortex/memory.hpp"

#include "cortex/quality.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numeric>
#include <unordered_set>

namespace cortex {
namespace {

constexpr std::string_view memory_magic = "CORTEX-MEMORY-1\n";
constexpr std::size_t maximum_dimension = 4'096U;
constexpr std::size_t maximum_records = 10'000'000U;
constexpr std::size_t maximum_text_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_indexed_terms = 192U;

[[nodiscard]] std::uint64_t content_hash(std::string_view text) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

[[nodiscard]] std::vector<std::string> indexed_terms(std::string_view text) {
    std::vector<std::string> terms = significant_words(text);
    if (terms.size() > maximum_indexed_terms) terms.resize(maximum_indexed_terms);
    return terms;
}

[[nodiscard]] float lexical_similarity(std::span<const std::string> query,
                                       std::span<const std::string> document) noexcept {
    if (query.empty() || document.empty()) return 0.0F;
    std::size_t left = 0U;
    std::size_t right = 0U;
    std::size_t overlap = 0U;
    while (left < query.size() && right < document.size()) {
        if (query[left] == document[right]) {
            ++overlap;
            ++left;
            ++right;
        } else if (query[left] < document[right]) {
            ++left;
        } else {
            ++right;
        }
    }
    if (overlap == 0U) return 0.0F;
    const float coverage = static_cast<float>(overlap) / static_cast<float>(query.size());
    const std::size_t union_size = query.size() + document.size() - overlap;
    const float jaccard = static_cast<float>(overlap)
                          / static_cast<float>(std::max<std::size_t>(union_size, 1U));
    return std::clamp(0.88F * coverage + 0.12F * jaccard, 0.0F, 1.0F);
}

} // namespace

LongTermMemory::LongTermMemory(std::size_t embedding_dimension)
    : dimension_(embedding_dimension) {
    if (dimension_ == 0U || dimension_ > maximum_dimension) {
        throw Error("dimensão da memória vetorial fora do intervalo seguro");
    }
}

LongTermMemory::LongTermMemory(LongTermMemory&& other) noexcept {
    std::unique_lock lock(other.mutex_);
    dimension_ = other.dimension_;
    next_id_ = other.next_id_;
    records_ = std::move(other.records_);
    content_index_ = std::move(other.content_index_);
}

LongTermMemory& LongTermMemory::operator=(LongTermMemory&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    std::scoped_lock lock(mutex_, other.mutex_);
    dimension_ = other.dimension_;
    next_id_ = other.next_id_;
    records_ = std::move(other.records_);
    content_index_ = std::move(other.content_index_);
    return *this;
}

std::uint64_t LongTermMemory::current_unix_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void LongTermMemory::validate_embedding(std::span<const float> embedding) const {
    if (embedding.size() != dimension_) {
        throw Error("embedding possui dimensão incompatível com a memória");
    }
    for (const float value : embedding) {
        if (!std::isfinite(value)) {
            throw Error("embedding contém valor não finito");
        }
    }
}

std::size_t LongTermMemory::size() const {
    std::shared_lock lock(mutex_);
    return records_.size();
}

std::uint64_t LongTermMemory::remember(std::string text,
                                       std::span<const float> embedding) {
    validate_embedding(embedding);
    if (text.empty()) {
        throw Error("não é permitido armazenar memória vazia");
    }
    if (text.size() > maximum_text_bytes) {
        throw Error("texto excede o limite por memória");
    }

    std::vector<float> normalized(embedding.begin(), embedding.end());
    normalize_in_place(normalized);
    std::unique_lock lock(mutex_);
    // Ingestões podem ser repetidas depois de reiniciar. Conteúdo idêntico mantém o id
    // original; o multimap evita uma varredura linear e confirma colisões pelo texto.
    const std::uint64_t hash = content_hash(text);
    const auto [first, last] = content_index_.equal_range(hash);
    for (auto candidate = first; candidate != last; ++candidate) {
        if (candidate->second < records_.size()
            && records_[candidate->second].text == text) {
            records_[candidate->second].embedding = std::move(normalized);
            return records_[candidate->second].id;
        }
    }
    if (records_.size() >= maximum_records) {
        throw Error("memória atingiu o limite de registros");
    }
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw Error("espaço de ids da memória esgotado");
    }

    const std::uint64_t id = next_id_++;
    std::vector<std::string> terms = indexed_terms(text);
    records_.push_back(Record{id, current_unix_ms(), std::move(text),
                              std::move(normalized), std::move(terms)});
    try {
        content_index_.emplace(hash, records_.size() - 1U);
    } catch (...) {
        records_.pop_back();
        --next_id_;
        throw;
    }
    return id;
}

bool LongTermMemory::erase(std::uint64_t id) {
    std::unique_lock lock(mutex_);
    const auto found = std::find_if(records_.begin(), records_.end(),
                                    [id](const Record& record) { return record.id == id; });
    if (found == records_.end()) {
        return false;
    }
    records_.erase(found);
    content_index_.clear();
    content_index_.reserve(records_.size());
    for (std::size_t index = 0U; index < records_.size(); ++index) {
        content_index_.emplace(content_hash(records_[index].text), index);
    }
    return true;
}

std::vector<MemoryHit> LongTermMemory::search(std::span<const float> query,
                                              std::size_t limit,
                                              float minimum_score) const {
    validate_embedding(query);
    if (!std::isfinite(minimum_score) || minimum_score < -1.0F || minimum_score > 1.0F) {
        throw Error("minimum_score deve estar entre -1 e 1");
    }
    if (limit == 0U) {
        return {};
    }

    std::vector<float> normalized_query(query.begin(), query.end());
    normalize_in_place(normalized_query);
    std::shared_lock lock(mutex_);
    if (records_.empty()) {
        return {};
    }

    // Cada thread escreve uma faixa exclusiva do vetor de scores, sem locks no hot path.
    std::vector<float> scores(records_.size(), 0.0F);
    auto calculate = [this, &normalized_query, &scores](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            scores[index] = dot_product(records_[index].embedding, normalized_query);
        }
    };
    global_thread_pool().parallel_for(0U, records_.size(), 256U, calculate);

    std::vector<std::size_t> indices(records_.size());
    std::iota(indices.begin(), indices.end(), 0U);
    const std::size_t wanted = std::min(limit, indices.size());
    std::partial_sort(indices.begin(),
                      indices.begin() + static_cast<std::ptrdiff_t>(wanted),
                      indices.end(),
                      [this, &scores](std::size_t left, std::size_t right) {
                          if (scores[left] != scores[right]) {
                              return scores[left] > scores[right];
                          }
                          return records_[left].id < records_[right].id;
                      });

    std::vector<MemoryHit> hits;
    hits.reserve(wanted);
    for (std::size_t rank = 0U; rank < wanted; ++rank) {
        const std::size_t index = indices[rank];
        if (scores[index] < minimum_score) {
            continue;
        }
        const Record& record = records_[index];
        hits.push_back(MemoryHit{record.id, record.created_unix_ms,
                                 scores[index], record.text});
    }
    return hits;
}

std::vector<MemoryHit> LongTermMemory::hybrid_search(
    std::string_view query_text,
    std::span<const float> query_embedding,
    std::size_t limit,
    float minimum_score) const {
    validate_embedding(query_embedding);
    if (query_text.empty()) throw Error("consulta híbrida não pode ser vazia");
    if (!std::isfinite(minimum_score) || minimum_score < -1.0F || minimum_score > 1.0F) {
        throw Error("minimum_score deve estar entre -1 e 1");
    }
    if (limit == 0U) return {};

    std::vector<float> normalized_query(query_embedding.begin(), query_embedding.end());
    normalize_in_place(normalized_query);
    const std::vector<std::string> query_terms = indexed_terms(query_text);
    std::shared_lock lock(mutex_);
    if (records_.empty()) return {};

    std::vector<float> scores(records_.size(), 0.0F);
    auto calculate = [this, &normalized_query, &query_terms, &scores](
                         std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            const float cosine = std::clamp(
                dot_product(records_[index].embedding, normalized_query), -1.0F, 1.0F);
            const float lexical = lexical_similarity(query_terms, records_[index].lexical_terms);
            // Um nome exato deve prevalecer sobre um embedding pequeno ainda pouco treinado.
            // Sem interseção lexical, o cosseno continua útil, mas recebe peso conservador.
            scores[index] = lexical > 0.0F
                ? std::clamp(0.28F * std::max(0.0F, cosine) + 0.72F * lexical, 0.0F, 1.0F)
                : cosine * 0.58F;
        }
    };
    global_thread_pool().parallel_for(0U, records_.size(), 256U, calculate);

    std::vector<std::size_t> indices(records_.size());
    std::iota(indices.begin(), indices.end(), 0U);
    const std::size_t wanted = std::min(limit, indices.size());
    std::partial_sort(indices.begin(),
                      indices.begin() + static_cast<std::ptrdiff_t>(wanted),
                      indices.end(),
                      [this, &scores](std::size_t left, std::size_t right) {
                          return scores[left] != scores[right]
                              ? scores[left] > scores[right]
                              : records_[left].id < records_[right].id;
                      });
    std::vector<MemoryHit> hits;
    hits.reserve(wanted);
    for (std::size_t rank = 0U; rank < wanted; ++rank) {
        const std::size_t index = indices[rank];
        if (scores[index] < minimum_score) continue;
        const Record& record = records_[index];
        hits.push_back(MemoryHit{
            record.id, record.created_unix_ms, scores[index], record.text});
    }
    return hits;
}

void LongTermMemory::save(const std::filesystem::path& path) const {
    const std::filesystem::path temporary = temporary_path_for(path);
    try {
        BinaryWriter writer(temporary);
        writer.magic(memory_magic);
        std::shared_lock lock(mutex_);
        writer.u32(static_cast<std::uint32_t>(dimension_));
        writer.u64(next_id_);
        writer.u64(static_cast<std::uint64_t>(records_.size()));
        for (const Record& record : records_) {
            writer.u64(record.id);
            writer.u64(record.created_unix_ms);
            writer.string(record.text);
            for (const float value : record.embedding) {
                writer.f32(value);
            }
        }
        writer.flush();
        lock.unlock();
        replace_file(temporary, path);
    } catch (...) {
        std::filesystem::remove(temporary);
        throw;
    }
}

LongTermMemory LongTermMemory::load(const std::filesystem::path& path,
                                    std::size_t expected_dimension) {
    BinaryReader reader(path);
    reader.expect_magic(memory_magic);
    const std::size_t dimension = static_cast<std::size_t>(reader.u32());
    if (expected_dimension != 0U && dimension != expected_dimension) {
        throw Error("dimensão da memória não corresponde ao modelo carregado");
    }
    LongTermMemory memory(dimension);
    memory.next_id_ = reader.u64();
    const std::uint64_t serialized_count = reader.u64();
    if (serialized_count > maximum_records) {
        throw Error("arquivo de memória declara registros demais");
    }
    const std::size_t count = static_cast<std::size_t>(serialized_count);
    memory.records_.reserve(count);
    memory.content_index_.reserve(count);
    std::unordered_set<std::uint64_t> seen_ids;
    seen_ids.reserve(count);
    std::uint64_t maximum_id = 0U;

    for (std::size_t index = 0U; index < count; ++index) {
        Record record;
        record.id = reader.u64();
        record.created_unix_ms = reader.u64();
        record.text = reader.string(maximum_text_bytes);
        record.embedding.resize(dimension);
        for (float& value : record.embedding) {
            value = reader.f32();
            if (!std::isfinite(value)) {
                throw Error("memória persistida contém embedding não finito");
            }
        }
        normalize_in_place(record.embedding);
        if (record.id == 0U || !seen_ids.insert(record.id).second || record.text.empty()) {
            throw Error("registro inválido ou id duplicado na memória persistida");
        }
        record.lexical_terms = indexed_terms(record.text);
        maximum_id = std::max(maximum_id, record.id);
        memory.records_.push_back(std::move(record));
        memory.content_index_.emplace(
            content_hash(memory.records_.back().text), memory.records_.size() - 1U);
    }
    if (memory.next_id_ <= maximum_id) {
        throw Error("contador next_id inválido na memória persistida");
    }
    reader.require_end();
    return memory;
}

} // namespace cortex
