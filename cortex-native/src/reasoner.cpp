#include "cortex/reasoner.hpp"

#include "cortex/knowledge.hpp"
#include "cortex/quality.hpp"
#include "cortex/tokenizer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace cortex {
namespace {

[[nodiscard]] char fold_latin_code_point(std::uint32_t code_point) noexcept {
    if (code_point >= static_cast<std::uint32_t>('A')
        && code_point <= static_cast<std::uint32_t>('Z')) {
        return static_cast<char>(code_point + 32U);
    }
    if ((code_point >= static_cast<std::uint32_t>('a')
         && code_point <= static_cast<std::uint32_t>('z'))
        || (code_point >= static_cast<std::uint32_t>('0')
            && code_point <= static_cast<std::uint32_t>('9'))) {
        return static_cast<char>(code_point);
    }
    switch (code_point) {
    case 0x00C0U: case 0x00C1U: case 0x00C2U: case 0x00C3U: case 0x00C4U:
    case 0x00E0U: case 0x00E1U: case 0x00E2U: case 0x00E3U: case 0x00E4U:
        return 'a';
    case 0x00C7U: case 0x00E7U:
        return 'c';
    case 0x00C8U: case 0x00C9U: case 0x00CAU: case 0x00CBU:
    case 0x00E8U: case 0x00E9U: case 0x00EAU: case 0x00EBU:
        return 'e';
    case 0x00CCU: case 0x00CDU: case 0x00CEU: case 0x00CFU:
    case 0x00ECU: case 0x00EDU: case 0x00EEU: case 0x00EFU:
        return 'i';
    case 0x00D1U: case 0x00F1U:
        return 'n';
    case 0x00D2U: case 0x00D3U: case 0x00D4U: case 0x00D5U: case 0x00D6U:
    case 0x00F2U: case 0x00F3U: case 0x00F4U: case 0x00F5U: case 0x00F6U:
        return 'o';
    case 0x00D9U: case 0x00DAU: case 0x00DBU: case 0x00DCU:
    case 0x00F9U: case 0x00FAU: case 0x00FBU: case 0x00FCU:
        return 'u';
    default:
        return '\0';
    }
}

[[nodiscard]] std::uint32_t next_code_point(std::string_view text,
                                            std::size_t& offset) noexcept {
    const unsigned char first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80U) return first;

    std::uint32_t value = 0U;
    std::size_t continuation_count = 0U;
    if ((first & 0xE0U) == 0xC0U) {
        value = first & 0x1FU;
        continuation_count = 1U;
    } else if ((first & 0xF0U) == 0xE0U) {
        value = first & 0x0FU;
        continuation_count = 2U;
    } else if ((first & 0xF8U) == 0xF0U) {
        value = first & 0x07U;
        continuation_count = 3U;
    } else {
        return 0xFFFDU;
    }
    if (offset + continuation_count > text.size()) {
        offset = text.size();
        return 0xFFFDU;
    }
    for (std::size_t index = 0U; index < continuation_count; ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[offset]);
        if ((byte & 0xC0U) != 0x80U) return 0xFFFDU;
        ++offset;
        value = (value << 6U) | (byte & 0x3FU);
    }
    return value;
}

[[nodiscard]] std::string normalize_words(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool pending_space = false;
    std::size_t offset = 0U;
    while (offset < text.size()) {
        const std::uint32_t code_point = next_code_point(text, offset);
        const char folded = fold_latin_code_point(code_point);
        if (folded != '\0') {
            if (pending_space && !normalized.empty()) normalized.push_back(' ');
            normalized.push_back(folded);
            pending_space = false;
        } else {
            pending_space = !normalized.empty();
        }
    }
    return normalized;
}

[[nodiscard]] bool contains_phrase(std::string_view normalized,
                                   std::string_view phrase) {
    if (normalized == phrase) return true;
    std::string padded;
    padded.reserve(normalized.size() + 2U);
    padded.push_back(' ');
    padded.append(normalized);
    padded.push_back(' ');
    const std::string needle = " " + std::string(phrase) + " ";
    return padded.find(needle) != std::string::npos;
}

[[nodiscard]] bool contains_any(std::string_view normalized,
                                std::initializer_list<std::string_view> phrases) {
    return std::any_of(phrases.begin(), phrases.end(), [&](std::string_view phrase) {
        return contains_phrase(normalized, phrase);
    });
}

[[nodiscard]] std::vector<std::string> split_words(std::string_view normalized) {
    std::vector<std::string> words;
    std::size_t begin = 0U;
    while (begin < normalized.size()) {
        const std::size_t end = normalized.find(' ', begin);
        const std::size_t length = (end == std::string_view::npos ? normalized.size() : end)
                                   - begin;
        if (length != 0U) words.emplace_back(normalized.substr(begin, length));
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return words;
}

[[nodiscard]] bool generic_follow_up(std::string_view normalized) {
    return contains_any(normalized, {
        "explique mais", "continue", "continue explicando", "como assim",
        "e como funciona", "por que", "pode detalhar", "detalhe mais"
    }) && split_words(normalized).size() <= 4U;
}

[[nodiscard]] std::string_view last_user_turn(std::string_view conversation) noexcept {
    constexpr std::array<std::string_view, 3U> user_markers{
        "Usuário:", "Usuario:", "User:"
    };
    std::size_t latest = std::string_view::npos;
    std::size_t content_begin = 0U;
    for (const std::string_view marker : user_markers) {
        const std::size_t found = conversation.rfind(marker);
        if (found != std::string_view::npos
            && (latest == std::string_view::npos || found > latest)) {
            latest = found;
            content_begin = found + marker.size();
        }
    }
    if (latest == std::string_view::npos) return {};
    while (content_begin < conversation.size()
           && (conversation[content_begin] == ' ' || conversation[content_begin] == '\t')) {
        ++content_begin;
    }
    std::size_t content_end = conversation.size();
    for (const std::string_view marker : {
             std::string_view{"\nAssistente:"}, std::string_view{"\nAssistant:"},
             std::string_view{"\nCortex:"}, std::string_view{"\nUsuário:"},
             std::string_view{"\nUsuario:"}, std::string_view{"\nUser:"}}) {
        const std::size_t found = conversation.find(marker, content_begin);
        if (found != std::string_view::npos) content_end = std::min(content_end, found);
    }
    return conversation.substr(content_begin, content_end - content_begin);
}

[[nodiscard]] std::string_view last_assistant_turn(std::string_view conversation) noexcept {
    constexpr std::array<std::string_view, 3U> assistant_markers{
        "Assistente:", "Assistant:", "Cortex:"
    };
    std::size_t latest = std::string_view::npos;
    std::size_t content_begin = 0U;
    for (const std::string_view marker : assistant_markers) {
        const std::size_t found = conversation.rfind(marker);
        if (found != std::string_view::npos
            && (latest == std::string_view::npos || found > latest)) {
            latest = found;
            content_begin = found + marker.size();
        }
    }
    if (latest == std::string_view::npos) return {};
    while (content_begin < conversation.size()
           && (conversation[content_begin] == ' ' || conversation[content_begin] == '\t')) {
        ++content_begin;
    }
    std::size_t content_end = conversation.size();
    for (const std::string_view marker : {
             std::string_view{"\nUsuário:"}, std::string_view{"\nUsuario:"},
             std::string_view{"\nUser:"}, std::string_view{"\nAssistente:"},
             std::string_view{"\nAssistant:"}, std::string_view{"\nCortex:"}}) {
        const std::size_t found = conversation.find(marker, content_begin);
        if (found != std::string_view::npos) content_end = std::min(content_end, found);
    }
    while (content_end > content_begin
           && (conversation[content_end - 1U] == '\r'
               || conversation[content_end - 1U] == '\n')) {
        --content_end;
    }
    return conversation.substr(content_begin, content_end - content_begin);
}

[[nodiscard]] std::string_view text_after_colon(std::string_view prompt) noexcept {
    const std::size_t colon = prompt.find(':');
    if (colon == std::string_view::npos) return {};
    std::size_t begin = colon + 1U;
    while (begin < prompt.size()
           && (prompt[begin] == ' ' || prompt[begin] == '\t'
               || prompt[begin] == '\r' || prompt[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = prompt.size();
    while (end > begin
           && (prompt[end - 1U] == ' ' || prompt[end - 1U] == '\t'
               || prompt[end - 1U] == '\r' || prompt[end - 1U] == '\n')) {
        --end;
    }
    return prompt.substr(begin, end - begin);
}

class ExpressionParser final {
public:
    explicit ExpressionParser(std::string_view expression) : expression_(expression) {}

    [[nodiscard]] double parse() {
        const double value = parse_sum();
        skip_spaces();
        if (offset_ != expression_.size()) fail("símbolo inesperado");
        if (!std::isfinite(value)) fail("resultado não finito");
        return value;
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error(std::string(message));
    }

    void skip_spaces() noexcept {
        while (offset_ < expression_.size()
               && (expression_[offset_] == ' ' || expression_[offset_] == '\t')) {
            ++offset_;
        }
    }

    [[nodiscard]] bool consume(char character) noexcept {
        skip_spaces();
        if (offset_ < expression_.size() && expression_[offset_] == character) {
            ++offset_;
            return true;
        }
        return false;
    }

    [[nodiscard]] double parse_sum() {
        double value = parse_product();
        while (true) {
            if (consume('+')) value = checked(value + parse_product());
            else if (consume('-')) value = checked(value - parse_product());
            else return value;
        }
    }

    [[nodiscard]] double parse_product() {
        double value = parse_power();
        while (true) {
            if (consume('*')) {
                value = checked(value * parse_power());
            } else if (consume('/')) {
                const double divisor = parse_power();
                if (divisor == 0.0) fail("divisão por zero");
                value = checked(value / divisor);
            } else if (consume('%')) {
                const double divisor = parse_power();
                if (divisor == 0.0) fail("módulo por zero");
                value = checked(std::fmod(value, divisor));
            } else {
                return value;
            }
        }
    }

    [[nodiscard]] double parse_power() {
        double base = parse_unary();
        if (consume('^')) {
            const double exponent = parse_power();
            if (std::abs(exponent) > 1'024.0) fail("expoente fora do limite");
            base = checked(std::pow(base, exponent));
        }
        return base;
    }

    [[nodiscard]] double parse_unary() {
        if (consume('+')) return parse_unary();
        if (consume('-')) return checked(-parse_unary());
        return parse_primary();
    }

    [[nodiscard]] double parse_primary() {
        if (consume('(')) {
            const double value = parse_sum();
            if (!consume(')')) fail("parêntese não fechado");
            return value;
        }
        skip_spaces();
        const std::size_t begin = offset_;
        double integer = 0.0;
        bool has_digit = false;
        while (offset_ < expression_.size() && expression_[offset_] >= '0'
               && expression_[offset_] <= '9') {
            has_digit = true;
            integer = checked(integer * 10.0
                              + static_cast<double>(expression_[offset_] - '0'));
            ++offset_;
        }
        double fraction = 0.0;
        double scale = 1.0;
        if (offset_ < expression_.size()
            && (expression_[offset_] == '.' || expression_[offset_] == ',')) {
            ++offset_;
            while (offset_ < expression_.size() && expression_[offset_] >= '0'
                   && expression_[offset_] <= '9') {
                has_digit = true;
                scale *= 0.1;
                fraction += static_cast<double>(expression_[offset_] - '0') * scale;
                ++offset_;
            }
        }
        if (!has_digit || offset_ == begin) fail("número esperado");
        return checked(integer + fraction);
    }

    [[nodiscard]] double checked(double value) const {
        if (!std::isfinite(value)) fail("resultado fora do limite numérico");
        return value;
    }

    std::string_view expression_;
    std::size_t offset_ = 0U;
};

[[nodiscard]] std::optional<std::string> extract_expression(std::string_view prompt,
                                                            std::string_view normalized) {
    const bool requested = contains_any(normalized, {
        "calcule", "calcular", "quanto e", "qual o resultado", "resolva",
        "resultado de"
    });
    std::string expression;
    expression.reserve(std::min<std::size_t>(prompt.size(), 256U));
    bool started = false;
    bool binary_operator = false;
    for (std::size_t index = 0U; index < prompt.size();) {
        const unsigned char byte = static_cast<unsigned char>(prompt[index]);
        if (index + 1U < prompt.size() && byte == 0xC3U) {
            const unsigned char second = static_cast<unsigned char>(prompt[index + 1U]);
            if (second == 0x97U || second == 0xB7U) { // × ou ÷
                if (started) {
                    expression.push_back(second == 0x97U ? '*' : '/');
                    binary_operator = true;
                }
                index += 2U;
                continue;
            }
        }
        const char character = static_cast<char>(byte);
        const bool digit = character >= '0' && character <= '9';
        const bool allowed = digit || character == ' ' || character == '\t'
                             || character == '.' || character == ','
                             || character == '+' || character == '-' || character == '*'
                             || character == '/' || character == '%' || character == '^'
                             || character == '(' || character == ')';
        if (!started) {
            const bool signed_number = (character == '+' || character == '-')
                                       && index + 1U < prompt.size()
                                       && (prompt[index + 1U] >= '0'
                                           && prompt[index + 1U] <= '9');
            if (digit || character == '(' || signed_number) {
                started = true;
                expression.push_back(character);
            }
        } else if (allowed) {
            expression.push_back(character);
            if (character == '+' || character == '-' || character == '*'
                || character == '/' || character == '%' || character == '^') {
                binary_operator = true;
            }
        } else if (character == '?' || character == '=' || character == ':') {
            // Pontuação final encerra a expressão para não concatenar números posteriores.
            break;
        } else {
            break;
        }
        ++index;
        if (expression.size() > 256U) return std::nullopt;
    }
    while (!expression.empty() && (expression.back() == ' ' || expression.back() == '\t')) {
        expression.pop_back();
    }
    if (!started || !binary_operator) return std::nullopt;

    // Uma expressão sem pedido explícito precisa ocupar toda a entrada, salvo pontuação.
    if (!requested) {
        for (const char character : prompt) {
            const bool allowed = (character >= '0' && character <= '9') || character == ' '
                                 || character == '\t' || character == '.' || character == ','
                                 || character == '+' || character == '-' || character == '*'
                                 || character == '/' || character == '%' || character == '^'
                                 || character == '(' || character == ')' || character == '?'
                                 || character == '=';
            if (!allowed && static_cast<unsigned char>(character) < 0x80U) return std::nullopt;
        }
    }
    return expression;
}

[[nodiscard]] std::string format_number(double value) {
    if (value == 0.0) value = 0.0; // normaliza -0.
    std::ostringstream output;
    const double rounded = std::round(value);
    const double tolerance = 1.0e-12 * std::max(1.0, std::abs(value));
    if (std::abs(value - rounded) <= tolerance
        && std::abs(rounded) <= 9'000'000'000'000'000.0) {
        output << std::fixed << std::setprecision(0) << rounded;
    } else {
        output << std::setprecision(12) << value;
    }
    return output.str();
}

[[nodiscard]] std::size_t requested_benchmark_steps(std::string_view normalized) {
    for (const std::string& word : split_words(normalized)) {
        std::size_t parsed = 0U;
        const auto result = std::from_chars(word.data(), word.data() + word.size(), parsed);
        if (result.ec == std::errc{} && result.ptr == word.data() + word.size()) {
            return std::clamp(parsed, std::size_t{16U}, std::size_t{2'048U});
        }
    }
    return 128U;
}

[[nodiscard]] NativeAnswer benchmark_answer(std::string_view normalized,
                                            const NeuralLanguageModel& model) {
    const std::size_t steps = requested_benchmark_steps(normalized);
    NeuralLanguageModel::State state = model.zero_state();
    const std::size_t vocabulary = model.config().vocabulary_size;
    std::vector<float> logits;
    double checksum = 0.0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0U; index < steps; ++index) {
        const TokenId token = static_cast<TokenId>(vocabulary > BpeTokenizer::first_merge_id
            ? BpeTokenizer::first_merge_id + index % (vocabulary - BpeTokenizer::first_merge_id)
            : index % vocabulary);
        logits = model.step(token, state);
        if (!logits.empty()) checksum += static_cast<double>(logits[index % logits.size()]);
    }
    const double seconds = std::max(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
        1.0e-9);
    if (!std::isfinite(checksum)) {
        return NativeAnswer{"O benchmark foi interrompido porque o modelo produziu valores não finitos.",
                            LocalResponseMode::benchmark};
    }

    std::ostringstream answer;
    answer << "Benchmark local concluído.\n"
           << "- Motor medido: GRU " << model.config().layer_count << 'x'
           << model.config().hidden_size << " em C++20\n"
           << "- Parâmetros: " << model.parameter_count() << "\n"
           << "- Passos autoregressivos: " << steps << "\n"
           << "- Tempo: " << std::fixed << std::setprecision(2) << seconds * 1'000.0
           << " ms\n- Desempenho: " << std::setprecision(1)
           << static_cast<double>(steps) / seconds << " passos neurais/s\n"
           << "Este número mede o forward do fallback GRU neste hardware; não é a velocidade "
              "de um modelo GGUF.";
    return NativeAnswer{answer.str(), LocalResponseMode::benchmark};
}

[[nodiscard]] std::string compact_memory_text(std::string_view text) {
    constexpr std::size_t maximum = 360U;
    std::string compact;
    compact.reserve(std::min(text.size(), maximum));
    bool previous_space = false;
    for (const char character : text) {
        const bool space = character == '\r' || character == '\n'
                           || character == '\t' || character == ' ';
        if (space) {
            if (!previous_space && !compact.empty()) compact.push_back(' ');
            previous_space = true;
        } else {
            compact.push_back(character);
            previous_space = false;
        }
        if (compact.size() >= maximum) break;
    }
    if (text.size() > maximum) compact += "…";
    return compact;
}

[[nodiscard]] NativeAnswer memory_answer(std::span<const MemoryHit> memories) {
    if (memories.empty()) {
        return NativeAnswer{
            "Ainda não encontrei uma memória relevante para essa pergunta. Você pode ensinar um "
            "fato com a memória automática da conversa ou com o comando `cortex remember`.",
            LocalResponseMode::memory};
    }
    std::ostringstream answer;
    answer << "Encontrei estas memórias semanticamente próximas (elas são registros, não fatos "
              "verificados):";
    const std::size_t limit = std::min<std::size_t>(memories.size(), 3U);
    for (std::size_t index = 0U; index < limit; ++index) {
        answer << "\n- #" << memories[index].id << ": "
               << compact_memory_text(memories[index].text);
    }
    return NativeAnswer{answer.str(), LocalResponseMode::memory};
}

[[nodiscard]] std::set<std::string> content_words(std::string_view text) {
    static const std::set<std::string, std::less<>> stop_words{
        "a", "ao", "aos", "as", "como", "com", "da", "das", "de", "do", "dos",
        "e", "em", "essa", "esse", "esta", "este", "eu", "faz", "funciona", "me",
        "busque", "internet", "meu", "minha", "na", "nas", "no", "nos", "o", "os",
        "foi", "para", "pesquise", "pesquisar", "por", "procure", "qual", "que", "quem",
        "se", "ser", "sobre", "um", "uma", "voce", "web", "the", "what", "how", "is"
    };
    std::set<std::string> result;
    for (std::string word : split_words(normalize_words(text))) {
        if (word.size() >= 2U && !stop_words.contains(word)) result.insert(std::move(word));
    }
    return result;
}

[[nodiscard]] std::optional<NativeAnswer> recalled_web_knowledge(
    std::string_view prompt,
    std::span<const MemoryHit> memories) {
    const std::set<std::string> query_words = content_words(prompt);
    if (query_words.empty()) return std::nullopt;

    const MemoryHit* best = nullptr;
    std::size_t best_overlap = 0U;
    for (const MemoryHit& memory : memories) {
        if (!std::string_view(memory.text).starts_with("[WEB]")) continue;
        const std::set<std::string> memory_words = content_words(memory.text);
        std::size_t overlap = 0U;
        for (const std::string& word : query_words) {
            if (memory_words.contains(word)) ++overlap;
        }
        if (overlap > best_overlap) {
            best = &memory;
            best_overlap = overlap;
        }
    }
    const std::size_t required_overlap = query_words.size() >= 2U ? 2U : 1U;
    if (best == nullptr || best_overlap < required_overlap) return std::nullopt;

    constexpr std::size_t prefix_size = 5U;
    constexpr std::size_t maximum_answer_bytes = 2'048U;
    std::string stored = best->text.substr(prefix_size);
    if (stored.size() > maximum_answer_bytes) {
        std::size_t boundary = maximum_answer_bytes;
        while (boundary > 0U
               && (static_cast<unsigned char>(stored[boundary]) & 0xC0U) == 0x80U) {
            --boundary;
        }
        stored.resize(boundary);
        stored += "…";
    }
    while (!stored.empty() && (stored.front() == '\r' || stored.front() == '\n'
                               || stored.front() == ' ' || stored.front() == '\t')) {
        stored.erase(0U, 1U);
    }
    return NativeAnswer{
        "Conhecimento recuperado de uma pesquisa anterior:\n" + stored,
        LocalResponseMode::web_knowledge};
}

} // namespace

WebResearchRequest parse_web_research_request(std::string_view prompt) {
    // Além do texto normalizado, conservamos o byte original de cada caractere
    // dobrado. Assim reconhecemos maiúsculas/acentos/pontuação sem destruir uma
    // consulta como "C++" ou alterar o que será enviado à fonte HTTPS.
    std::string normalized;
    std::vector<std::size_t> original_offsets;
    normalized.reserve(prompt.size());
    original_offsets.reserve(prompt.size());
    bool pending_space = false;
    std::size_t offset = 0U;
    while (offset < prompt.size()) {
        const std::size_t original_offset = offset;
        const char folded = fold_latin_code_point(next_code_point(prompt, offset));
        if (folded != '\0') {
            if (pending_space && !normalized.empty()) {
                normalized.push_back(' ');
                original_offsets.push_back(original_offset);
            }
            normalized.push_back(folded);
            original_offsets.push_back(original_offset);
            pending_space = false;
        } else {
            pending_space = !normalized.empty();
        }
    }

    constexpr std::array<std::string_view, 20U> prefixes{
        "voce pode pesquisar ", "pesquise na internet por ", "pesquise na internet ", "pesquise na web por ",
        "pesquise sobre ", "pesquise por ", "pesquise ",
        "pesquisar na internet por ", "pesquisar na internet ", "pesquisar ",
        "procure na internet por ", "procure na internet ", "procure sobre ",
        "procure por ", "procure ", "busque na internet por ", "busque na internet ",
        "busque por ", "busque ", "search "
    };
    for (const std::string_view prefix : prefixes) {
        const std::string_view command = prefix.substr(0U, prefix.size() - 1U);
        if (normalized != command && !normalized.starts_with(prefix)) continue;

        // Avança a partir do último caractere do comando no texto original, não
        // do primeiro caractere normalizável da consulta. Isso também preserva
        // consultas compostas somente por emoji ou alfabetos não latinos.
        std::size_t query_begin = original_offsets[command.size() - 1U];
        static_cast<void>(next_code_point(prompt, query_begin));
        while (query_begin < prompt.size()) {
            const char character = prompt[query_begin];
            if (character != ' ' && character != '\t' && character != '\r'
                && character != '\n' && character != ':' && character != ','
                && character != ';' && character != '=') {
                break;
            }
            ++query_begin;
        }
        std::size_t query_end = prompt.size();
        while (query_end > query_begin) {
            const char character = prompt[query_end - 1U];
            if (character != ' ' && character != '\t' && character != '\r'
                && character != '\n') {
                break;
            }
            --query_end;
        }
        return WebResearchRequest{
            true, std::string(prompt.substr(query_begin, query_end - query_begin))};
    }
    if (normalized == "pesquise" || normalized == "pesquisar"
        || normalized == "procure" || normalized == "busque"
        || normalized == "search") {
        return WebResearchRequest{true, {}};
    }
    return WebResearchRequest{false, {}};
}

std::optional<NativeAnswer> reason_locally(std::string_view prompt,
                                           std::string_view conversation_context,
                                           const NeuralLanguageModel& model,
                                           std::span<const MemoryHit> memories) {
    const std::string normalized_prompt = normalize_words(prompt);
    if (normalized_prompt.empty()) return std::nullopt;

    const WebResearchRequest research_request = parse_web_research_request(prompt);
    if (research_request.explicit_request) {
        if (std::optional<NativeAnswer> web = recalled_web_knowledge(prompt, memories)) {
            return web;
        }
        return NativeAnswer{
            research_request.query.empty()
                ? "Diga o que devo pesquisar. Exemplo: `pesquise na internet por computação "
                  "quântica`."
                : "Reconheci o pedido de pesquisa. O transporte HTTPS sem chave está disponível "
                  "no servidor web (`cortex serve`); este ciclo local não abrirá a rede nem "
                  "inventará uma resposta.",
            LocalResponseMode::safe_fallback};
    }

    if (contains_any(normalized_prompt, {
            "analise seu comportamento", "analise sua resposta", "autoanalise",
            "avalie sua resposta", "como foi sua resposta"
        })) {
        const std::string_view previous_answer = last_assistant_turn(conversation_context);
        const std::string_view previous_question = last_user_turn(conversation_context);
        if (previous_answer.empty()) {
            return NativeAnswer{
                "Ainda não há uma resposta anterior no contexto para analisar. Depois de uma resposta, "
                "peça `analise sua resposta` e eu medirei relevância, coerência, repetição e estrutura.",
                LocalResponseMode::self_analysis};
        }
        return NativeAnswer{
            format_response_analysis(previous_question, previous_answer),
            LocalResponseMode::self_analysis};
    }

    if (contains_any(normalized_prompt, {
            "analise este texto", "analise o texto", "analisar este texto",
            "analisar texto", "analyze this text"
        })) {
        const std::string_view subject = text_after_colon(prompt);
        if (subject.empty()) {
            return NativeAnswer{
                "Envie o conteúdo depois de dois-pontos. Exemplo: `analise este texto: ...`.",
                LocalResponseMode::text_analysis};
        }
        return NativeAnswer{format_text_analysis(subject), LocalResponseMode::text_analysis};
    }

    std::string intent = normalized_prompt;
    if (generic_follow_up(normalized_prompt) && !conversation_context.empty()) {
        const std::string_view previous_question = last_user_turn(conversation_context);
        if (!previous_question.empty()) {
            intent = normalize_words(previous_question) + ' ' + normalized_prompt;
        }
    }

    if (contains_any(normalized_prompt, {
            "benchmark", "teste de desempenho", "medir desempenho", "medir velocidade",
            "quantos tokens por segundo"
        })) {
        return benchmark_answer(normalized_prompt, model);
    }

    if (const std::optional<std::string> expression = extract_expression(prompt, normalized_prompt)) {
        try {
            const double result = ExpressionParser(*expression).parse();
            return NativeAnswer{"Resultado: " + *expression + " = " + format_number(result),
                                LocalResponseMode::calculation};
        } catch (const std::exception& exception) {
            return NativeAnswer{"Não posso calcular essa expressão: "
                                    + std::string(exception.what()) + '.',
                                LocalResponseMode::calculation};
        }
    }

    if (contains_any(normalized_prompt, {
            "o que voce lembra", "voce lembra", "lembra de mim", "minhas memorias",
            "o que sabe sobre mim", "recorda de mim"
        })) {
        return memory_answer(memories);
    }

    const std::size_t word_count = split_words(normalized_prompt).size();
    if (word_count <= 5U && contains_any(normalized_prompt, {
            "oi", "ola", "bom dia", "boa tarde", "boa noite", "e ai"
        })) {
        return NativeAnswer{
            "Olá! Estou funcionando em modo local confiável. Posso executar um benchmark, "
            "resolver cálculos, pesquisar a internet com fontes ou consultar memórias.",
            LocalResponseMode::knowledge};
    }
    if (word_count <= 6U && contains_any(normalized_prompt, {
            "obrigado", "obrigada", "valeu", "agradeco"
        })) {
        return NativeAnswer{"Disponha. Qual é o próximo objetivo?", LocalResponseMode::knowledge};
    }

    if (normalized_prompt == "status" || normalized_prompt == "teste"
        || normalized_prompt == "cortex" || contains_any(normalized_prompt, {
            "voce esta funcionando", "esta funcionando", "voce esta online",
            "cortex esta online", "sistema funcionando"
        })) {
        std::ostringstream status;
        status << "Sim. O núcleo local está funcionando com reasoner, filtro de qualidade, "
               << curated_knowledge_size() << " temas curados e GRU "
               << model.config().layer_count << 'x' << model.config().hidden_size << " de "
               << model.parameter_count() << " parâmetros auxiliares. O Cognitive Mesh também "
                  "oferece síntese, resumo, escrita, ideação e código; no X40, `--x40` ativa IBM "
                  "Granite 4.2 3B local.";
        return NativeAnswer{status.str(), LocalResponseMode::knowledge};
    }

    // Pedidos de composição precisam chegar ao Cognitive Mesh/GGUF. Sem este bloqueio,
    // uma palavra como "segurança" ou "concorrência" acionaria uma resposta curta sobre
    // a própria Cortex e ignoraria verbos como escreva, compare, planeje ou implemente.
    const bool requires_synthesis = contains_any(intent, {
        "escreva", "redija", "crie", "gere", "implemente", "implementar", "refatore",
        "planeje", "raciocine", "resolva", "compare", "brainstorm", "liste ideias",
        "sugira ideias", "ideias para", "alternativas para", "sugestoes para", "resuma",
        "sintetize", "artigo", "ensaio", "conto", "poema", "historia criativa",
        "conte uma historia"
    });
    if (requires_synthesis) return std::nullopt;

    if (contains_any(intent, {"inteligencia artificial", "machine learning", "aprendizado de maquina"})) {
        return NativeAnswer{
            "Inteligência artificial é o campo que constrói sistemas capazes de executar tarefas "
            "associadas a percepção, linguagem, decisão ou previsão. Machine learning é uma parte "
            "desse campo: em vez de programar todas as regras, parâmetros são ajustados a partir de "
            "dados e uma função de erro. Isso não torna toda saída verdadeira; ela ainda precisa de "
            "validação e fontes.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"rede neural", "redes neurais", "neuronio artificial"})
        && !contains_phrase(intent, "gru")) {
        return NativeAnswer{
            "Uma rede neural aplica camadas de transformações parametrizadas a vetores. Durante o "
            "treino, gradientes indicam como alterar os pesos para reduzir uma função de erro. A "
            "arquitetura e os dados determinam o que ela consegue representar; quantidade de "
            "parâmetros sozinha não garante raciocínio nem fatos corretos.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {
            "resposta aleatoria", "respostas aleatorias", "sem logica", "resposta errada",
            "inventando resposta", "alucinacao"
        })) {
        return NativeAnswer{
            "O GRU pequeno prevê o próximo token e pode gerar uma frase fluente sem relação com a "
            "pergunta. Agora respostas verificáveis passam primeiro pelo raciocínio nativo e a "
            "saída neural desconectada é descartada em vez de ser apresentada como correta.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {
            "quem e voce", "o que e cortex", "seu nome", "quem voce e"
        })) {
        std::ostringstream identity;
        identity << "Eu sou Cortex, um assistente local em C++20. Uso regras verificáveis e "
                    "ferramentas nativas para tarefas determinísticas, memória vetorial para "
                    "contexto e um GRU de " << model.parameter_count()
                 << " parâmetros como componente auxiliar. Modelos GGUF locais podem ampliar o "
                    "conhecimento sem usar API.";
        return NativeAnswer{identity.str(), LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {
            "o que voce faz", "suas capacidades", "como pode ajudar", "lista de comandos",
            "me ajude", "ajuda", "help"
        })) {
        return NativeAnswer{
            "Posso, sem chave: executar benchmark e cálculos; sintetizar explicações, comparações, "
            "resumos, ideias, histórias e alguns programas pelo Cognitive Mesh; analisar textos; "
            "ingerir documentos; guardar memórias; pesquisar com fontes; e avaliar meu comportamento. "
            "No notebook X40, `--x40` ativa IBM Granite 4.2 3B local para geração moderna. A "
            "autoevolução audita e propõe código, mas aplicação e rollback exigem confirmação humana.",
            LocalResponseMode::knowledge};
    }

    const std::size_t mentioned_components =
        static_cast<std::size_t>(contains_phrase(intent, "bpe"))
        + static_cast<std::size_t>(contains_phrase(intent, "gru"))
        + static_cast<std::size_t>(contains_phrase(intent, "memoria"));
    const bool mentions_components = mentioned_components >= 2U;
    if (mentions_components || contains_any(intent, {
            "arquitetura", "como cortex funciona", "componentes do cortex", "resuma o cortex"
        })) {
        return NativeAnswer{
            "A arquitetura possui sete caminhos: BPE/GRU auxiliar; embedding lexical-hash; memória "
            "híbrida; base curada e documentos ingeridos; Cognitive Mesh com especialistas; pesquisa "
            "HTTPS; e um GGUF aberto opcional. No X40, IBM Granite 4.2 3B é o gerador. O orquestrador "
            "C++ escolhe Flash, Medium ou Alta potência e filtra relevância, coerência e repetição; "
            "Alta potência pode executar uma segunda revisão.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"sem api", "nao quero api", "precisa de api", "chave de api"})) {
        return NativeAnswer{
            "Cortex não precisa de API. Cognitive Mesh, benchmark, cálculos, memória, documentos, "
            "servidor e pesquisa funcionam no executável C++20. Para geração geral moderna no X40, "
            "`cortex serve --state state --x40` inicia IBM Granite 4.2 3B no próprio computador, sem "
            "chave ou envio do chat a um provedor externo.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {
            "modelo local", "modelo gguf", "qwen", "llama cpp", "qual modelo", "seu modelo",
            "modelo voce usa", "mais inteligente", "melhorar inteligencia", "parametros"
        })) {
        std::ostringstream model_description;
        model_description << "Sem GGUF, o modo confiável combina Cognitive Mesh, documentos, base "
                             "curada, memória híbrida e o GRU auxiliar de " << model.parameter_count()
                          << " parâmetros. `cortex serve --state state --x40` ativa IBM Granite 4.2 "
                             "3B Q4 via runtime C++ local, sem chave, ajustado para Core i5 com pelo "
                             "menos 6 GiB de RAM. Outros GGUF estão disponíveis em `cortex models`.";
        return NativeAnswer{model_description.str(), LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"memoria", "lembranca", "recordacao", "busca semantica"})) {
        return NativeAnswer{
            "A memória fica fora dos pesos neurais. Cortex gera um vetor normalizado, compara-o por "
            "similaridade de cosseno e persiste cada episódio em arquivo binário. Assim, lembrar um "
            "fato não exige retreinar o modelo. Use `cortex remember` para gravar e `cortex recall` "
            "para consultar.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"bpe", "tokenizacao", "tokenizador", "utf 8"})) {
        return NativeAnswer{
            "O tokenizador BPE começa com os 256 valores de byte e aprende a unir pares frequentes. "
            "Como todo byte continua representável, qualquer UTF-8 pode ser reconstruído exatamente; "
            "os merges apenas reduzem o comprimento das sequências comuns.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"gru", "rede recorrente", "estado oculto", "gate"})) {
        return NativeAnswer{
            "A GRU mantém um estado oculto por camada. O gate de reset controla quanto do passado "
            "entra no estado candidato; o gate de atualização mistura esse candidato com o estado "
            "anterior. Na Cortex pequena, ela é útil para embeddings e fallback limitado, não para "
            "fingir conhecimento geral.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"bptt", "adam", "treinamento", "como aprende", "gradiente"})) {
        return NativeAnswer{
            "No treinamento, a entropia cruzada mede o erro do próximo token. BPTT aplica a regra da "
            "cadeia aos estados recorrentes em ordem inversa, o clipping limita gradientes extremos e "
            "Adam atualiza os pesos com médias móveis do primeiro e segundo momentos.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"concorrencia", "paralelismo", "threads", "desempenho"})) {
        return NativeAnswer{
            "Cortex usa um pool persistente de threads para matrizes e busca vetorial e outro conjunto "
            "de workers para HTTP. Matrizes row-major contíguas favorecem cache e SIMD. Execute "
            "`benchmark` no chat para medir agora o forward do GRU neste hardware.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"internet", "pesquisa online", "pesquisar na web", "duckduckgo"})) {
        return NativeAnswer{
            "A internet é opcional. Um comando explícito pesquisa mesmo com o checkbox desligado: "
            "DuckDuckGo é a primeira fonte e Wikipedia em português é o fallback. A resposta registra "
            "nome e URL HTTPS e entra na memória `[WEB]`; sem resultado objetivo, Cortex não inventa "
            "um fato recente.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {
            "autoedicao", "mudar codigo", "alterar codigo", "editar codigo",
            "automodificar", "autoprogramacao"
        })) {
        return NativeAnswer{
            "A autoevolução possui dois níveis. O padrão limita fontes do projeto. Com "
            "`--unrestricted-self-edit --host 127.0.0.1`, Cortex pode substituir ou criar arquivos "
            "em qualquer caminho aceito pela conta/Windows e preparar comandos livres de validação. "
            "Cada operação permanece visível e pede `APLICAR ID` ou `EXECUTAR ID`; o Windows ainda "
            "pode solicitar UAC. Backups, hash e rollback evitam alterações silenciosas.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"seguranca", "seguro", "validacao"})) {
        return NativeAnswer{
            "As entradas HTTP têm limites, o JSON é validado, URLs remotas exigem HTTPS, segredos não "
            "entram na interface e o llama-server fica em 127.0.0.1. Checkpoints validam assinatura e "
            "dimensões antes de alocar memória; autoedição exige confirmação humana.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"python", "node js", "javascript", "linguagem c", "c 20"})) {
        return NativeAnswer{
            "O núcleo, treinamento, inferência, memória, servidor HTTP e gerenciamento de processos "
            "são C++20 compilado. Não há Python, npm ou runtime Node.js. A interface contém somente "
            "JavaScript de navegador embutido no executável, sem etapa de build web.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"recursao", "funcao recursiva", "algoritmo recursivo"})) {
        return NativeAnswer{
            "Recursão ocorre quando uma função resolve um problema chamando a si mesma para uma "
            "entrada menor. Ela precisa de um caso-base que encerre as chamadas. Exemplo: fatorial "
            "pode usar `fat(n) = n * fat(n-1)` e `fat(0) = 1`. Sem caso-base, a pilha cresce até "
            "estourar; para entradas grandes, uma versão iterativa pode consumir menos memória.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"ceu e azul", "ceu azul", "porque o ceu"})) {
        return NativeAnswer{
            "Durante o dia, moléculas da atmosfera espalham mais intensamente os comprimentos de onda "
            "curtos da luz solar. Esse espalhamento de Rayleigh distribui luz azul pelo céu em todas "
            "as direções. No nascer e no pôr do sol, o caminho atmosférico é maior e sobram tons "
            "vermelhos e alaranjados na direção do Sol.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"roblox nao abre", "roblox nao inicia", "erro no roblox",
                              "roblox travando", "tela branca roblox"})) {
        return NativeAnswer{
            "Para diagnosticar o Roblox sem modificar o Player: feche apenas processos Roblox, "
            "verifique internet e espaço em disco, atualize o driver gráfico e tente a reinstalação "
            "oficial. O launcher deste repositório também consegue analisar logs e reparar caches, "
            "mas deve pedir confirmação antes de remover dados locais. Preserve conta, itens e "
            "progresso, que ficam nos servidores do Roblox.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"conte uma piada", "me conte uma piada", "piada de programador"})) {
        return NativeAnswer{
            "Piada de programador: havia dois bugs caminhando pelo código. O primeiro disse: “isso é "
            "um recurso”. O segundo respondeu: “ótimo, então documenta você”.",
            LocalResponseMode::knowledge};
    }

    if (std::optional<NativeAnswer> web = recalled_web_knowledge(prompt, memories)) {
        return web;
    }
    if (std::optional<CuratedKnowledgeAnswer> curated = lookup_curated_knowledge(intent)) {
        return NativeAnswer{std::move(curated->text), LocalResponseMode::curated_knowledge};
    }
    return std::nullopt;
}

bool neural_response_is_relevant(std::string_view prompt, std::string_view response) {
    // Um único avaliador agora protege tanto o GRU quanto modelos GGUF: ele combina
    // aderência lexical, estrutura, repetição e vazamento de marcadores de diálogo.
    return assess_response(prompt, response).accepted;
}

NativeAnswer safe_local_fallback() {
    return NativeAnswer{
        "Não encontrei evidência local suficiente para responder a esse fato sem inventar. Posso "
        "pesquisar uma fonte, ingerir um documento, executar cálculos/análise ou organizar um plano "
        "verificável. Para geração geral moderna no notebook X40 sem chave, inicie IBM Granite "
        "4.2 3B com `cortex serve --state state --x40`.",
        LocalResponseMode::safe_fallback};
}

std::string_view local_response_mode_name(LocalResponseMode mode) noexcept {
    switch (mode) {
    case LocalResponseMode::neural: return "local-gru";
    case LocalResponseMode::knowledge: return "local-reasoner";
    case LocalResponseMode::calculation: return "local-calculator";
    case LocalResponseMode::benchmark: return "local-benchmark";
    case LocalResponseMode::memory: return "local-memory";
    case LocalResponseMode::web_knowledge: return "local-web-knowledge";
    case LocalResponseMode::curated_knowledge: return "local-curated-knowledge";
    case LocalResponseMode::text_analysis: return "local-text-analysis";
    case LocalResponseMode::self_analysis: return "local-self-analysis";
    case LocalResponseMode::cognitive_synthesis: return "local-cognitive-mesh";
    case LocalResponseMode::safe_fallback: return "local-safe";
    }
    return "local-safe";
}

} // namespace cortex
