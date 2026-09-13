#include "cortex/memory.hpp"

#include "cortex/quality.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

[[nodiscard]] bool is_low_memory_available() noexcept {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        // Se restar menos de 800 MB, evita construir índice pesado
        return status.ullAvailPhys < (800ULL * 1024ULL * 1024ULL);
    }
    return false;
#else
    // Linux/macOS: tenta ler /proc/meminfo, falha silenciosa = assume memória ok
    try {
        std::ifstream meminfo("/proc/meminfo");
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.rfind("MemAvailable:", 0) == 0U) {
                std::size_t kb = 0U;
                std::sscanf(line.c_str(), "MemAvailable: %zu", &kb);
                return kb * 1024ULL < (800ULL * 1024ULL * 1024ULL);
            }
        }
    } catch (...) {}
    return false;
#endif
}

[[nodiscard]] bool would_exceed_memory_budget(std::size_t record_count, std::size_t dimension) noexcept {
    // Estimativa conservadora: embeddings + índices ~ N*dim*4*1.3
    const std::size_t embedding_bytes = record_count * dimension * sizeof(float);
    const std::size_t overhead = embedding_bytes / 3U;
    const std::size_t total = embedding_bytes + overhead;
    // Windows 10 com 4 GB: não passa de 1.2 GB para ANN; 8 GB: 2 GB
    constexpr std::size_t budget = 1200ULL * 1024ULL * 1024ULL;
    return total > budget || is_low_memory_available();
}

} // namespace

std::size_t LongTermMemory::ann_target_clusters(std::size_t record_count) noexcept {
    if (record_count < ann_threshold_) return 0U;
    const double root = std::sqrt(static_cast<double>(record_count));
    std::size_t clusters = static_cast<std::size_t>(root);
    clusters = std::clamp<std::size_t>(clusters, ann_min_clusters_, ann_max_clusters_);
    // Para N muito grande, limita crescimento para manter rebuild rápido
    if (record_count > 100000U) clusters = std::min<std::size_t>(clusters, 256U);
    return clusters;
}

void LongTermMemory::mark_ann_dirty_locked() noexcept {
    ann_dirty_ = true;
}

void LongTermMemory::rebuild_ann_locked() const {
    // Exige unique_lock de mutex_ já adquirido
    const std::size_t n = records_.size();
    ann_centroids_.clear();
    ann_postings_.clear();
    ann_built_upon_ = n;
    ann_dirty_ = false;
    lexical_inverted_index_.clear();
    if (n == 0U) return;
    // Guarda contra OOM no Windows 10 (4-8 GB + HD lento):
    // se o orçamento estourar, mantém busca exata (que é segura) e evita travar.
    const std::size_t dim = dimension_;
    if (would_exceed_memory_budget(n, dim)) {
        // Não constrói ANN pesado; híbrido usará busca exata linear
        // Para N moderado ainda tenta índice lexical reduzido
        if (n < 50000U) {
            try {
                lexical_inverted_index_.reserve(std::min<std::size_t>(n * 2U, 200000U));
                for (std::size_t idx = 0U; idx < n; ++idx) {
                    for (const std::string& term : records_[idx].lexical_terms) {
                        auto it = lexical_inverted_index_.find(term);
                        if (it == lexical_inverted_index_.end()) {
                            // Limita crescimento: ignora termos muito raros em escala gigante
                            if (lexical_inverted_index_.size() > 100000U) continue;
                        }
                        lexical_inverted_index_[term].push_back(idx);
                    }
                }
            } catch (const std::bad_alloc&) {
                lexical_inverted_index_.clear();
            }
        }
        return;
    }
    // Reconstrói índice lexical invertido para híbrido em escala
    // Para N > 100k, limita para não estourar RAM (usa apenas termos mais frequentes)
    try {
        if (n > 100000U) {
            // N muito grande: não constrói invertido completo, híbrido usará ANN + fallback
        } else {
            lexical_inverted_index_.reserve(n * 2U);
            for (std::size_t idx = 0U; idx < n; ++idx) {
                for (const std::string& term : records_[idx].lexical_terms) {
                    lexical_inverted_index_[term].push_back(idx);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        lexical_inverted_index_.clear();
        ann_centroids_.clear();
        ann_postings_.clear();
        return;
    }
    if (n < ann_threshold_) return;
    const std::size_t k = ann_target_clusters(n);
    if (k == 0U || k > n) return;
    // Inicializa centroides amostrando registros de forma determinística
    try {
        Random rng(static_cast<std::uint64_t>(n * 0x9E3779B97F4A7C15ULL + dim * 31ULL + 0xC07ECAFEULL));
        ann_centroids_.assign(k, std::vector<float>(dim, 0.0F));
        ann_postings_.assign(k, {});
        // k-means++ simplificado: escolhe k registros distintos aleatoriamente
        std::unordered_set<std::size_t> chosen;
        chosen.reserve(k * 2U);
        for (std::size_t c = 0U; c < k; ++c) {
            std::size_t pick = 0U;
            for (int attempt = 0; attempt < 100; ++attempt) {
                pick = static_cast<std::size_t>(rng.next_u64() % n);
                if (chosen.insert(pick).second) break;
            }
            ann_centroids_[c] = records_[pick].embedding;
        }
        // Iterações de k-means: adapta quantidade ao tamanho
        const std::size_t iterations = n > 100000U ? 2U : (n > 10000U ? 3U : 5U);
        std::vector<std::size_t> assignments(n, 0U);
        std::vector<std::vector<float>> new_centroids(k, std::vector<float>(dim, 0.0F));
        std::vector<std::size_t> counts(k, 0U);
        for (std::size_t iter = 0U; iter < iterations; ++iter) {
            // Zera acumuladores
            for (auto& c : new_centroids) std::fill(c.begin(), c.end(), 0.0F);
            std::fill(counts.begin(), counts.end(), 0U);
            // Atribuição
            auto assign_range = [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    const auto& emb = records_[i].embedding;
                    std::size_t best = 0U;
                    float best_score = -2.0F;
                    for (std::size_t c = 0U; c < k; ++c) {
                        float s = dot_product(emb, ann_centroids_[c]);
                        if (s > best_score) {
                            best_score = s;
                            best = c;
                        }
                    }
                    assignments[i] = best;
                }
            };
            global_thread_pool().parallel_for(0U, n, 1024U, assign_range);
            // Acumula
            for (std::size_t i = 0U; i < n; ++i) {
                const std::size_t c = assignments[i];
                ++counts[c];
                const auto& emb = records_[i].embedding;
                auto& acc = new_centroids[c];
                for (std::size_t d = 0U; d < dim; ++d) acc[d] += emb[d];
            }
            // Recomputa centroides
            for (std::size_t c = 0U; c < k; ++c) {
                if (counts[c] == 0U) continue;
                const float inv = 1.0F / static_cast<float>(counts[c]);
                for (std::size_t d = 0U; d < dim; ++d) new_centroids[c][d] *= inv;
                normalize_in_place(new_centroids[c]);
                ann_centroids_[c] = new_centroids[c];
            }
        }
        // Construção final das postings
        for (auto& p : ann_postings_) p.clear();
        for (std::size_t i = 0U; i < n; ++i) {
            const std::size_t c = assignments[i];
            ann_postings_[c].push_back(i);
        }
    } catch (const std::bad_alloc&) {
        // Em Windows 10 com pouca RAM/HD, evita travar: desativa ANN e usa exato
        ann_centroids_.clear();
        ann_postings_.clear();
        lexical_inverted_index_.clear();
    }
}

void LongTermMemory::ensure_ann_built() const {
    // Caminho rápido: já pronto e sem sujeira
    {
        std::shared_lock<std::shared_mutex> shared(mutex_);
        if (!ann_dirty_ && ann_built_upon_ == records_.size() && !ann_centroids_.empty()) return;
        if (records_.size() < ann_threshold_) return;
        if (!ann_dirty_ && ann_built_upon_ == records_.size()) return;
    }
    // Precisa reconstruir: upgrade para unique
    std::unique_lock<std::shared_mutex> unique(mutex_);
    if (!ann_dirty_ && ann_built_upon_ == records_.size() && !ann_centroids_.empty()) return;
    if (records_.size() < ann_threshold_) {
        // Limpa estruturas se caiu abaixo do limiar
        if (!ann_centroids_.empty() || !ann_postings_.empty()) {
            ann_centroids_.clear();
            ann_postings_.clear();
            ann_built_upon_ = records_.size();
            ann_dirty_ = false;
        }
        return;
    }
    // Reconstrói se dirty ou tamanho mudou significativamente (>10% ou >512)
    const bool size_changed = ann_built_upon_ == 0U || records_.size() > ann_built_upon_ + 512U
                              || records_.size() * 10U < ann_built_upon_ * 9U
                              || records_.size() * 10U > ann_built_upon_ * 11U;
    if (ann_dirty_ || size_changed || ann_centroids_.empty()) {
        rebuild_ann_locked();
    }
}

std::vector<std::size_t> LongTermMemory::ann_candidates(std::span<const float> normalized_query) const {
    // Exige que ANN já esteja construído e que shared_lock esteja ativo
    const std::size_t k = ann_centroids_.size();
    if (k == 0U || ann_postings_.empty()) return {};
    // Calcula scores aos centroides
    std::vector<float> centroid_scores(k, 0.0F);
    auto calc = [&](std::size_t begin, std::size_t end) {
        for (std::size_t c = begin; c < k && c < end; ++c) {
            centroid_scores[c] = dot_product(normalized_query, ann_centroids_[c]);
        }
    };
    global_thread_pool().parallel_for(0U, k, 16U, calc);
    // Ordena centroides por score
    std::vector<std::size_t> order(k);
    std::iota(order.begin(), order.end(), 0U);
    std::partial_sort(order.begin(), order.begin() + std::min<std::size_t>(k, 32U), order.end(),
                      [&](std::size_t a, std::size_t b){ return centroid_scores[a] > centroid_scores[b]; });
    // nprobe adaptativo: garante cobertura mínima
    std::size_t nprobe = std::clamp<std::size_t>(k / 4U, 4U, 32U);
    // Para k pequeno, probeia mais
    if (k <= 32U) nprobe = std::min<std::size_t>(k, 8U);
    nprobe = std::min(nprobe, k);
    // Coleta candidatos
    std::size_t total = 0U;
    for (std::size_t i = 0U; i < nprobe; ++i) total += ann_postings_[order[i]].size();
    std::vector<std::size_t> candidates;
    candidates.reserve(total + 64U);
    for (std::size_t i = 0U; i < nprobe; ++i) {
        const auto& posting = ann_postings_[order[i]];
        candidates.insert(candidates.end(), posting.begin(), posting.end());
    }
    // Se candidatos ainda são poucos (< 2*threshold), expande nprobe
    if (candidates.size() < ann_threshold_ / 2U && nprobe < k) {
        std::size_t extra = std::min<std::size_t>(k - nprobe, 8U);
        for (std::size_t i = nprobe; i < nprobe + extra; ++i) {
            const auto& posting = ann_postings_[order[i]];
            candidates.insert(candidates.end(), posting.begin(), posting.end());
        }
    }
    return candidates;
}

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
    lexical_inverted_index_ = std::move(other.lexical_inverted_index_);
    ann_centroids_ = std::move(other.ann_centroids_);
    ann_postings_ = std::move(other.ann_postings_);
    ann_built_upon_ = other.ann_built_upon_;
    ann_dirty_ = other.ann_dirty_;
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
    lexical_inverted_index_ = std::move(other.lexical_inverted_index_);
    ann_centroids_ = std::move(other.ann_centroids_);
    ann_postings_ = std::move(other.ann_postings_);
    ann_built_upon_ = other.ann_built_upon_;
    ann_dirty_ = other.ann_dirty_;
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
    std::vector<std::string> terms = indexed_terms(text);
    std::unique_lock lock(mutex_);
    // Deduplicação exata por hash (rápida)
    const std::uint64_t hash = content_hash(text);
    const auto [first, last] = content_index_.equal_range(hash);
    for (auto candidate = first; candidate != last; ++candidate) {
        if (candidate->second < records_.size()
            && records_[candidate->second].text == text) {
            records_[candidate->second].embedding = normalized;
            records_[candidate->second].lexical_terms = terms;
            // Atualiza índice lexical invertido para o registro reescrito
            // Marca dirty para reconstrução lazy; não reconstrói imediatamente para manter remember O(1)
            mark_ann_dirty_locked();
            return records_[candidate->second].id;
        }
    }
    // Deduplicação semântica em escala: se ANN já existe, verifica近 duplicata
    if (records_.size() >= ann_threshold_ && !ann_centroids_.empty() && !ann_dirty_) {
        // Busca rápida entre centroides para achar cluster mais próximo
        std::size_t best_cluster = 0U;
        float best_score = -2.0F;
        for (std::size_t c = 0U; c < ann_centroids_.size(); ++c) {
            float s = dot_product(normalized, ann_centroids_[c]);
            if (s > best_score) { best_score = s; best_cluster = c; }
        }
        if (best_score > 0.90F) {
            for (std::size_t idx : ann_postings_[best_cluster]) {
                if (idx >= records_.size()) continue;
                const Record& rec = records_[idx];
                float cos = dot_product(rec.embedding, normalized);
                if (cos > 0.995F) {
                    float lex = lexical_similarity(terms, rec.lexical_terms);
                    if (lex > 0.90F) {
                        // Considera duplicata semântica: atualiza existente
                        records_[idx].embedding = normalized;
                        // Mantém texto original para preservar id, mas atualiza termos
                        mark_ann_dirty_locked();
                        return rec.id;
                    }
                }
            }
        }
    }
    if (records_.size() >= maximum_records) {
        throw Error("memória atingiu o limite de registros");
    }
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw Error("espaço de ids da memória esgotado");
    }

    const std::uint64_t id = next_id_++;
    records_.push_back(Record{id, current_unix_ms(), text,
                              normalized, terms});
    try {
        content_index_.emplace(hash, records_.size() - 1U);
        // Atualiza índice lexical incrementalmente
        for (const std::string& term : terms) {
            lexical_inverted_index_[term].push_back(records_.size() - 1U);
        }
        mark_ann_dirty_locked();
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
    // Reconstrói índices derivados: content_index_ e lexical invertido
    // Para 10M, reconstruir é O(N). Mantém simplicidade e corretude;
    // ANN será marcado dirty e reconstruído lazy.
    content_index_.clear();
    content_index_.reserve(records_.size());
    lexical_inverted_index_.clear();
    lexical_inverted_index_.reserve(records_.size() * 2U);
    for (std::size_t index = 0U; index < records_.size(); ++index) {
        content_index_.emplace(content_hash(records_[index].text), index);
        for (const std::string& term : records_[index].lexical_terms) {
            lexical_inverted_index_[term].push_back(index);
        }
    }
    // Ajusta ANN: invalida se ainda havia
    if (!ann_centroids_.empty() || !ann_postings_.empty()) {
        mark_ann_dirty_locked();
        // Se apagou poucos itens, poderia ajustar postings incrementalmente,
        // mas reconstrução lazy é mais simples e segura.
        if (records_.size() < ann_threshold_) {
            ann_centroids_.clear();
            ann_postings_.clear();
            ann_built_upon_ = records_.size();
            ann_dirty_ = false;
        }
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

    // Garante ANN pronto se em escala
    if (size() >= ann_threshold_) {
        ensure_ann_built();
    }

    std::shared_lock lock(mutex_);
    if (records_.empty()) {
        return {};
    }

    // Caminho ANN para grande escala
    if (records_.size() >= ann_threshold_ && !ann_centroids_.empty() && !ann_dirty_) {
        std::vector<std::size_t> candidates = ann_candidates(normalized_query);
        if (!candidates.empty() && candidates.size() < records_.size()) {
            // Re-ranking exato somente nos candidatos
            std::vector<float> scores(candidates.size(), 0.0F);
            auto calc = [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) {
                    const std::size_t rec_idx = candidates[i];
                    if (rec_idx < records_.size()) {
                        scores[i] = dot_product(records_[rec_idx].embedding, normalized_query);
                    }
                }
            };
            global_thread_pool().parallel_for(0U, candidates.size(), 256U, calc);
            std::vector<std::size_t> order(candidates.size());
            std::iota(order.begin(), order.end(), 0U);
            const std::size_t wanted = std::min(limit, order.size());
            std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(wanted), order.end(),
                              [&](std::size_t a, std::size_t b){
                                  if (scores[a] != scores[b]) return scores[a] > scores[b];
                                  return records_[candidates[a]].id < records_[candidates[b]].id;
                              });
            std::vector<MemoryHit> hits;
            hits.reserve(wanted);
            for (std::size_t r = 0U; r < wanted; ++r) {
                const std::size_t oi = order[r];
                if (scores[oi] < minimum_score) continue;
                const std::size_t rec_idx = candidates[oi];
                const Record& rec = records_[rec_idx];
                hits.push_back(MemoryHit{rec.id, rec.created_unix_ms, scores[oi], rec.text});
            }
            // Se ANN retornou poucos hits (recall baixo) e minimum_score baixo, fallback para exato
            if (hits.size() < std::min<std::size_t>(limit, 3U) && minimum_score <= 0.0F && candidates.size() < records_.size()) {
                // fallback será feito fora deste bloco, mas evita retornar vazio incorreto
                // Continua para busca exata se ANN falhou em achar o top-1
                if (hits.empty() && limit > 0U) {
                    lock.unlock();
                    // Recai no caminho exato sem ANN (evita deadlock)
                    std::shared_lock lock2(mutex_);
                    std::vector<float> all_scores(records_.size(), 0.0F);
                    auto all_calc = [&](std::size_t b, std::size_t e){
                        for (std::size_t i=b;i<e;++i) all_scores[i]=dot_product(records_[i].embedding, normalized_query);
                    };
                    global_thread_pool().parallel_for(0U, records_.size(), 256U, all_calc);
                    std::vector<std::size_t> idx(records_.size());
                    std::iota(idx.begin(), idx.end(), 0U);
                    const std::size_t w = std::min(limit, idx.size());
                    std::partial_sort(idx.begin(), idx.begin()+w, idx.end(),
                                      [&](std::size_t a,std::size_t b){
                                          if(all_scores[a]!=all_scores[b]) return all_scores[a]>all_scores[b];
                                          return records_[a].id < records_[b].id;
                                      });
                    hits.clear();
                    for(std::size_t r=0;r<w;++r){
                        std::size_t i=idx[r];
                        if(all_scores[i] < minimum_score) continue;
                        hits.push_back(MemoryHit{records_[i].id, records_[i].created_unix_ms, all_scores[i], records_[i].text});
                    }
                    return hits;
                }
            }
            return hits;
        }
    }

    // Caminho exato (baseline) para pequenos N ou fallback
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

    if (size() >= ann_threshold_) {
        ensure_ann_built();
    }

    std::shared_lock lock(mutex_);
    if (records_.empty()) return {};

    // Para híbrido em escala, usa ANN + lexical para reduzir candidatos, mas garante
    // que matches lexicais exatos não sejam perdidos.
    if (records_.size() >= ann_threshold_ && !ann_centroids_.empty() && !ann_dirty_) {
        std::vector<std::size_t> ann_cands = ann_candidates(normalized_query);
        // Coleta candidatos lexicais via índice invertido
        std::unordered_set<std::size_t> seen;
        seen.reserve(ann_cands.size() + 1024U);
        for (std::size_t c : ann_cands) seen.insert(c);
        for (const std::string& term : query_terms) {
            auto it = lexical_inverted_index_.find(term);
            if (it != lexical_inverted_index_.end()) {
                for (std::size_t idx : it->second) {
                    if (seen.size() > ann_cands.size() + 5000U) break; // limita expansão
                    seen.insert(idx);
                }
            }
        }
        std::vector<std::size_t> candidates(seen.begin(), seen.end());
        if (!candidates.empty() && candidates.size() < records_.size()) {
            std::vector<float> scores(candidates.size(), 0.0F);
            auto calc = [&](std::size_t b, std::size_t e){
                for(std::size_t i=b;i<e;++i){
                    std::size_t rec_idx = candidates[i];
                    if(rec_idx >= records_.size()) continue;
                    const float cos = std::clamp(dot_product(records_[rec_idx].embedding, normalized_query), -1.0F, 1.0F);
                    const float lex = lexical_similarity(query_terms, records_[rec_idx].lexical_terms);
                    scores[i] = lex > 0.0F ? std::clamp(0.28F * std::max(0.0F, cos) + 0.72F * lex, 0.0F, 1.0F) : cos * 0.58F;
                }
            };
            global_thread_pool().parallel_for(0U, candidates.size(), 256U, calc);
            std::vector<std::size_t> order(candidates.size());
            std::iota(order.begin(), order.end(), 0U);
            const std::size_t wanted = std::min(limit, order.size());
            std::partial_sort(order.begin(), order.begin()+wanted, order.end(),
                              [&](std::size_t a, std::size_t b){
                                  if(scores[a]!=scores[b]) return scores[a]>scores[b];
                                  return records_[candidates[a]].id < records_[candidates[b]].id;
                              });
            std::vector<MemoryHit> hits;
            hits.reserve(wanted);
            for(std::size_t r=0;r<wanted;++r){
                std::size_t oi = order[r];
                if(scores[oi] < minimum_score) continue;
                std::size_t rec_idx = candidates[oi];
                const Record& rec = records_[rec_idx];
                hits.push_back(MemoryHit{rec.id, rec.created_unix_ms, scores[oi], rec.text});
            }
            // Fallback se híbrido não achou lexical esperado (ex: 3 registros, query diverge vetorialmente)
            if (hits.empty() && !query_terms.empty()) {
                // Verifica se existe registro com lexical alto que foi perdido por ANN restrito
                // Faz busca exata completa nesse caso raro
                lock.unlock();
                std::shared_lock lock2(mutex_);
                std::vector<float> all_scores(records_.size(), 0.0F);
                auto all_calc = [&](std::size_t b,std::size_t e){
                    for(std::size_t i=b;i<e;++i){
                        const float cos = std::clamp(dot_product(records_[i].embedding, normalized_query), -1.0F, 1.0F);
                        const float lex = lexical_similarity(query_terms, records_[i].lexical_terms);
                        all_scores[i] = lex > 0.0F ? std::clamp(0.28F * std::max(0.0F, cos) + 0.72F * lex, 0.0F, 1.0F) : cos * 0.58F;
                    }
                };
                global_thread_pool().parallel_for(0U, records_.size(), 256U, all_calc);
                std::vector<std::size_t> idx(records_.size());
                std::iota(idx.begin(), idx.end(), 0U);
                const std::size_t w2 = std::min(limit, idx.size());
                std::partial_sort(idx.begin(), idx.begin()+w2, idx.end(),
                                  [&](std::size_t a,std::size_t b){
                                      if(all_scores[a]!=all_scores[b]) return all_scores[a]>all_scores[b];
                                      return records_[a].id < records_[b].id;
                                  });
                hits.clear();
                for(std::size_t r=0;r<w2;++r){
                    std::size_t i=idx[r];
                    if(all_scores[i] < minimum_score) continue;
                    hits.push_back(MemoryHit{records_[i].id, records_[i].created_unix_ms, all_scores[i], records_[i].text});
                }
                return hits;
            }
            return hits;
        }
    }

    std::vector<float> scores(records_.size(), 0.0F);
    auto calculate = [this, &normalized_query, &query_terms, &scores](
                         std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            const float cosine = std::clamp(
                dot_product(records_[index].embedding, normalized_query), -1.0F, 1.0F);
            const float lexical = lexical_similarity(query_terms, records_[index].lexical_terms);
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
        {
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
            writer.close();
        }
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
    memory.lexical_inverted_index_.reserve(count * 2U);
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
        for (const std::string& term : memory.records_.back().lexical_terms) {
            memory.lexical_inverted_index_[term].push_back(memory.records_.size() - 1U);
        }
    }
    if (memory.next_id_ <= maximum_id) {
        throw Error("contador next_id inválido na memória persistida");
    }
    reader.require_end();
    memory.ann_dirty_ = memory.records_.size() >= ann_threshold_;
    memory.ann_built_upon_ = 0U;
    return memory;
}

void LongTermMemory::rebuild_index() {
    std::unique_lock lock(mutex_);
    rebuild_ann_locked();
}

bool LongTermMemory::ann_enabled() const noexcept {
    std::shared_lock lock(mutex_);
    return records_.size() >= ann_threshold_ && !ann_centroids_.empty() && !ann_dirty_;
}

std::size_t LongTermMemory::ann_cluster_count() const noexcept {
    std::shared_lock lock(mutex_);
    return ann_centroids_.size();
}

std::size_t LongTermMemory::ann_candidate_count(std::span<const float> query) const {
    validate_embedding(query);
    std::vector<float> nq(query.begin(), query.end());
    normalize_in_place(nq);
    ensure_ann_built();
    std::shared_lock lock(mutex_);
    if (ann_centroids_.empty()) return 0U;
    auto cands = ann_candidates(nq);
    return cands.size();
}

} // namespace cortex
