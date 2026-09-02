#include "cortex/reasoner.hpp"

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
        status << "Sim. O núcleo local está funcionando com o Cortex Native Reasoner, GRU "
               << model.config().layer_count << 'x' << model.config().hidden_size << " e "
               << model.parameter_count() << " parâmetros neurais. Digite `benchmark` para medir "
                  "o forward ou uma expressão como `2*(8+3)` para testar a lógica.";
        return NativeAnswer{status.str(), LocalResponseMode::knowledge};
    }

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
            "Posso, sem API: executar `benchmark`; calcular expressões como `2*(8+3)`; explicar "
            "BPE, GRU, memória, concorrência e segurança; guardar e recuperar lembranças; pesquisar "
            "na internet quando essa opção estiver ligada; e conversar com um modelo GGUF executado "
            "localmente. Para evitar invenções, assumo quando o conhecimento local não é suficiente.",
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
            "A arquitetura possui quatro caminhos: (1) BPE converte UTF-8 em tokens; (2) o GRU "
            "produz embeddings e mantém o fallback leve; (3) a memória persistente recupera episódios "
            "por similaridade; e (4) o orquestrador C++ combina conversa, memória e internet antes de "
            "usar uma ferramenta nativa ou um GGUF local. Respostas probabilísticas passam por uma "
            "verificação de relevância.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"sem api", "nao quero api", "precisa de api", "chave de api"})) {
        return NativeAnswer{
            "Cortex não precisa de API. Benchmark, cálculos, memória, servidor e fallback funcionam "
            "no executável C++20. Para conhecimento geral mais amplo, `cortex serve --state state "
            "--local` inicia pesos GGUF no seu computador; isso não usa chave nem envia o chat a um "
            "provedor externo.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {
            "modelo local", "modelo gguf", "qwen", "llama cpp", "qual modelo", "seu modelo",
            "modelo voce usa", "mais inteligente", "melhorar inteligencia", "parametros"
        })) {
        std::ostringstream model_description;
        model_description << "Sem um GGUF ativo, o modo confiável combina raciocínio determinístico, "
                             "memória e o GRU carregado de " << model.parameter_count()
                          << " parâmetros. O comando `cortex models` mostra perfis Qwen3 de 0,6B a "
                             "30,5B; `cortex serve --state state --local` escolhe um pela RAM e o "
                             "executa localmente via llama.cpp, sem API.";
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
            "A internet é opcional e fica desligada por conversa até você ativá-la. A busca consulta "
            "por HTTPS o Instant Answer do DuckDuckGo, limita o tamanho da resposta e inclui a fonte. "
            "Sem resultado objetivo, Cortex não deve inventar um fato recente.",
            LocalResponseMode::knowledge};
    }

    if (contains_any(intent, {"autoedicao", "mudar codigo", "alterar codigo", "editar codigo"})) {
        return NativeAnswer{
            "A autoedição é controlada: primeiro é criada uma proposta para um arquivo permitido; "
            "depois uma pessoa revisa o conteúdo e digita uma confirmação específica. A aplicação "
            "gera backup e nunca recompila nem reinicia silenciosamente.",
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
    return std::nullopt;
}

bool neural_response_is_relevant(std::string_view prompt, std::string_view response) {
    if (response.size() < 12U || response.size() > 4'096U) return false;
    const std::string normalized_response = normalize_words(response);
    if (normalized_response.empty()) return false;
    if (contains_any(normalized_response, {
            "como posso ajudar", "estou pronta para responder", "posso explicar mas preciso",
            "diga sua pergunta", "qual assunto voce quer", "vou responder somente",
            "nao consegui formular"
        })) {
        return false;
    }
    if (response.find("Usuário:") != std::string_view::npos
        || response.find("Sistema:") != std::string_view::npos
        || response.find("Assistant:") != std::string_view::npos) {
        return false;
    }

    const std::set<std::string> query_words = content_words(prompt);
    const std::set<std::string> answer_words = content_words(response);
    if (query_words.empty() || answer_words.size() < 2U) return false;
    std::size_t overlap = 0U;
    for (const std::string& word : query_words) {
        if (answer_words.contains(word)) ++overlap;
    }
    const std::size_t required = query_words.size() >= 3U ? 2U : 1U;
    if (overlap < required) return false;

    // Repetição extrema costuma indicar colapso autoregressivo do checkpoint pequeno.
    const std::vector<std::string> sequence = split_words(normalized_response);
    std::set<std::string> unique(sequence.begin(), sequence.end());
    return sequence.size() < 12U || unique.size() * 3U >= sequence.size();
}

NativeAnswer safe_local_fallback() {
    return NativeAnswer{
        "Não encontrei conhecimento local confiável suficiente para responder a isso. Para não "
        "inventar, descartei a continuação probabilística do GRU. Posso executar benchmark, cálculos "
        "e explicar os componentes da Cortex agora. Para conhecimento geral amplo sem API, inicie "
        "um modelo GGUF local com `cortex serve --state state --local`.",
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
    case LocalResponseMode::safe_fallback: return "local-safe";
    }
    return "local-safe";
}

} // namespace cortex
