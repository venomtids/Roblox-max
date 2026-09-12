#pragma once

#include "cortex/core.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cortex {

// BPE byte-level: reversível para qualquer UTF-8 e treinado sem ICU ou tokenizador externo.
class BpeTokenizer final {
public:
    static constexpr TokenId pad_id = 0U;
    static constexpr TokenId bos_id = 1U;
    static constexpr TokenId eos_id = 2U;
    static constexpr TokenId unknown_id = 3U;
    static constexpr TokenId first_byte_id = 4U;
    static constexpr TokenId byte_count = 256U;
    static constexpr TokenId first_merge_id = first_byte_id + byte_count;

    struct Merge final {
        TokenId left = 0U;
        TokenId right = 0U;
        TokenId result = 0U;
    };

    BpeTokenizer();

    // Aprende os pares adjacentes mais frequentes e os substitui de forma gulosa.
    void train(std::string_view corpus,
               std::size_t maximum_merges,
               std::size_t minimum_frequency = 2U);

    [[nodiscard]] std::vector<TokenId> encode(std::string_view text,
                                               bool add_bos = false,
                                               bool add_eos = false) const;
    [[nodiscard]] std::string decode(std::span<const TokenId> tokens,
                                     bool skip_special = true) const;

    [[nodiscard]] std::size_t vocabulary_size() const noexcept { return pieces_.size(); }
    [[nodiscard]] std::size_t merge_count() const noexcept { return merges_.size(); }
    [[nodiscard]] const std::vector<Merge>& merges() const noexcept { return merges_; }

    void save(const std::filesystem::path& path) const;
    [[nodiscard]] static BpeTokenizer load(const std::filesystem::path& path);

private:
    void initialize_base_vocabulary();
    static void apply_merge(std::vector<TokenId>& sequence, const Merge& merge);

    // pieces_[id] contém os bytes originais; a decodificação é apenas uma concatenação.
    std::vector<std::string> pieces_;
    // A ordem do vetor também é o rank BPE usado durante encode.
    std::vector<Merge> merges_;
};

} // namespace cortex
