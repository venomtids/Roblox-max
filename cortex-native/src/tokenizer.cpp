#include "cortex/tokenizer.hpp"

#include <limits>
#include <unordered_map>

namespace cortex {
namespace {

constexpr std::string_view tokenizer_magic = "CORTEX-BPE-1\n";
constexpr std::size_t maximum_serialized_merges = 1'000'000U;

[[nodiscard]] std::uint64_t pair_key(TokenId left, TokenId right) {
    return (static_cast<std::uint64_t>(left) << 32U) | static_cast<std::uint64_t>(right);
}

[[nodiscard]] TokenId pair_left(std::uint64_t key) {
    return static_cast<TokenId>(key >> 32U);
}

[[nodiscard]] TokenId pair_right(std::uint64_t key) {
    return static_cast<TokenId>(key & 0xFFFF'FFFFULL);
}

} // namespace

BpeTokenizer::BpeTokenizer() {
    initialize_base_vocabulary();
}

void BpeTokenizer::initialize_base_vocabulary() {
    pieces_.clear();
    merges_.clear();
    pieces_.resize(first_merge_id);
    pieces_[pad_id] = "<pad>";
    pieces_[bos_id] = "<bos>";
    pieces_[eos_id] = "<eos>";
    pieces_[unknown_id] = "<unk>";

    // Cada possível byte, inclusive NUL, possui um token de base próprio.
    for (TokenId byte = 0U; byte < byte_count; ++byte) {
        pieces_[first_byte_id + byte] = std::string(1, static_cast<char>(byte));
    }
}

void BpeTokenizer::apply_merge(std::vector<TokenId>& sequence, const Merge& merge) {
    if (sequence.size() < 2U) {
        return;
    }

    std::size_t write = 0U;
    std::size_t read = 0U;
    while (read < sequence.size()) {
        const bool matches = read + 1U < sequence.size()
                             && sequence[read] == merge.left
                             && sequence[read + 1U] == merge.right;
        if (matches) {
            sequence[write++] = merge.result;
            read += 2U; // Substituição não sobreposta, igual ao BPE clássico.
        } else {
            sequence[write++] = sequence[read++];
        }
    }
    sequence.resize(write);
}

void BpeTokenizer::train(std::string_view corpus,
                         std::size_t maximum_merges,
                         std::size_t minimum_frequency) {
    if (corpus.empty()) {
        throw Error("o corpus do tokenizador não pode estar vazio");
    }
    if (minimum_frequency < 2U) {
        throw Error("minimum_frequency deve ser pelo menos 2");
    }
    if (maximum_merges > maximum_serialized_merges) {
        throw Error("quantidade de merges BPE excede o limite de segurança");
    }

    initialize_base_vocabulary();
    std::vector<TokenId> sequence;
    sequence.reserve(corpus.size());
    for (const unsigned char byte : corpus) {
        sequence.push_back(first_byte_id + static_cast<TokenId>(byte));
    }

    for (std::size_t iteration = 0U; iteration < maximum_merges; ++iteration) {
        std::unordered_map<std::uint64_t, std::size_t> frequencies;
        frequencies.reserve(sequence.size());
        for (std::size_t index = 0U; index + 1U < sequence.size(); ++index) {
            ++frequencies[pair_key(sequence[index], sequence[index + 1U])];
        }

        std::uint64_t best_pair = 0U;
        std::size_t best_frequency = 0U;
        // O desempate pelo menor par torna o treino bit-a-bit determinístico.
        for (const auto& [candidate, frequency] : frequencies) {
            if (frequency > best_frequency
                || (frequency == best_frequency && candidate < best_pair)) {
                best_pair = candidate;
                best_frequency = frequency;
            }
        }
        if (best_frequency < minimum_frequency) {
            break;
        }
        if (pieces_.size() >= static_cast<std::size_t>(std::numeric_limits<TokenId>::max())) {
            throw Error("vocabulário BPE excedeu a capacidade de TokenId");
        }

        const Merge merge{
            pair_left(best_pair),
            pair_right(best_pair),
            static_cast<TokenId>(pieces_.size())
        };
        if (merge.left >= pieces_.size() || merge.right >= pieces_.size()) {
            throw Error("estado interno inválido durante treinamento BPE");
        }

        pieces_.push_back(pieces_[merge.left] + pieces_[merge.right]);
        merges_.push_back(merge);
        apply_merge(sequence, merge);
    }
}

std::vector<TokenId> BpeTokenizer::encode(std::string_view text,
                                          bool add_bos,
                                          bool add_eos) const {
    std::vector<TokenId> result;
    result.reserve(text.size() + (add_bos ? 1U : 0U) + (add_eos ? 1U : 0U));
    if (add_bos) {
        result.push_back(bos_id);
    }
    for (const unsigned char byte : text) {
        result.push_back(first_byte_id + static_cast<TokenId>(byte));
    }

    // Aplicar merges na ordem aprendida equivale a respeitar o rank de cada par.
    const std::size_t prefix = add_bos ? 1U : 0U;
    const std::size_t suffix = add_eos ? 1U : 0U;
    if (prefix == 0U && suffix == 0U) {
        for (const Merge& merge : merges_) {
            apply_merge(result, merge);
        }
    } else {
        // Especiais são isolados para que nenhum merge atravesse BOS/EOS.
        std::vector<TokenId> body(result.begin() + static_cast<std::ptrdiff_t>(prefix),
                                  result.end());
        for (const Merge& merge : merges_) {
            apply_merge(body, merge);
        }
        result.clear();
        if (add_bos) {
            result.push_back(bos_id);
        }
        result.insert(result.end(), body.begin(), body.end());
        if (add_eos) {
            result.push_back(eos_id);
        }
    }
    return result;
}

std::string BpeTokenizer::decode(std::span<const TokenId> tokens,
                                 bool skip_special) const {
    std::size_t required = 0U;
    for (const TokenId token : tokens) {
        if (token < first_byte_id && skip_special) {
            continue;
        }
        if (token < pieces_.size()) {
            required += pieces_[token].size();
        } else {
            required += pieces_[unknown_id].size();
        }
    }

    std::string result;
    result.reserve(required);
    for (const TokenId token : tokens) {
        if (token < first_byte_id && skip_special) {
            continue;
        }
        result += token < pieces_.size() ? pieces_[token] : pieces_[unknown_id];
    }
    return result;
}

void BpeTokenizer::save(const std::filesystem::path& path) const {
    const std::filesystem::path temporary = temporary_path_for(path);
    try {
        {
            BinaryWriter writer(temporary);
            writer.magic(tokenizer_magic);
            writer.u32(static_cast<std::uint32_t>(merges_.size()));
            for (const Merge& merge : merges_) {
                writer.u32(merge.left);
                writer.u32(merge.right);
                writer.u32(merge.result);
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

BpeTokenizer BpeTokenizer::load(const std::filesystem::path& path) {
    BinaryReader reader(path);
    reader.expect_magic(tokenizer_magic);
    const std::size_t count = static_cast<std::size_t>(reader.u32());
    if (count > maximum_serialized_merges) {
        throw Error("arquivo BPE declara merges demais");
    }

    BpeTokenizer tokenizer;
    for (std::size_t index = 0U; index < count; ++index) {
        const Merge merge{reader.u32(), reader.u32(), reader.u32()};
        const TokenId expected_id = static_cast<TokenId>(tokenizer.pieces_.size());
        if (merge.result != expected_id
            || merge.left >= expected_id
            || merge.right >= expected_id) {
            throw Error("grafo de merges BPE inválido no checkpoint");
        }
        tokenizer.pieces_.push_back(tokenizer.pieces_[merge.left]
                                    + tokenizer.pieces_[merge.right]);
        tokenizer.merges_.push_back(merge);
    }
    reader.require_end();
    return tokenizer;
}

} // namespace cortex
