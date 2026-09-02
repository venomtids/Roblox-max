#include "cortex/quality.hpp"

#include "cortex/core.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cortex {
namespace {

[[nodiscard]] char fold_latin(std::uint32_t codepoint) noexcept {
    if (codepoint >= static_cast<std::uint32_t>('A')
        && codepoint <= static_cast<std::uint32_t>('Z')) {
        return static_cast<char>(codepoint + 32U);
    }
    if ((codepoint >= static_cast<std::uint32_t>('a')
         && codepoint <= static_cast<std::uint32_t>('z'))
        || (codepoint >= static_cast<std::uint32_t>('0')
            && codepoint <= static_cast<std::uint32_t>('9'))) {
        return static_cast<char>(codepoint);
    }
    switch (codepoint) {
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

[[nodiscard]] std::uint32_t next_codepoint(std::string_view text,
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
        value = (value << 6U) | static_cast<std::uint32_t>(byte & 0x3FU);
    }
    return value;
}

[[nodiscard]] std::vector<std::string> normalized_words(std::string_view text) {
    std::vector<std::string> words;
    std::string current;
    std::size_t offset = 0U;
    while (offset < text.size()) {
        const std::uint32_t codepoint = next_codepoint(text, offset);
        const char folded = fold_latin(codepoint);
        if (folded != '\0') {
            current.push_back(folded);
        } else if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) words.push_back(std::move(current));
    return words;
}

[[nodiscard]] bool contains_role_marker(std::string_view response) {
    constexpr std::string_view markers[]{
        "Usuário:", "Usuario:", "User:", "Sistema:", "System:", "Assistant:"
    };
    for (const std::string_view marker : markers) {
        if (response.starts_with(marker)
            || response.find("\n" + std::string(marker)) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool transformation_request(std::string_view prompt) {
    constexpr std::string_view commands[]{
        "traduza", "traduzir", "traducao", "translate", "translation", "converta", "reescreva"
    };
    const std::vector<std::string> words = normalized_words(prompt);
    return std::any_of(words.begin(), words.end(), [&](const std::string& word) {
        return std::any_of(std::begin(commands), std::end(commands), [&](std::string_view command) {
            return word == command;
        });
    });
}

[[nodiscard]] bool contains_generic_evasion(std::string_view response) {
    std::string normalized;
    for (const std::string& word : normalized_words(response)) {
        if (!normalized.empty()) normalized.push_back(' ');
        normalized += word;
    }
    constexpr std::string_view phrases[]{
        "como posso ajudar", "estou pronta para responder", "diga sua pergunta",
        "qual assunto voce quer", "vou responder somente", "nao consegui formular"
    };
    return std::any_of(std::begin(phrases), std::end(phrases), [&](std::string_view phrase) {
        return normalized.find(phrase) != std::string::npos;
    });
}

[[nodiscard]] std::size_t paragraph_count(std::string_view text) {
    std::size_t paragraphs = 0U;
    bool paragraph_has_text = false;
    bool previous_line_blank = true;
    std::size_t line_begin = 0U;
    while (line_begin <= text.size()) {
        const std::size_t line_end = text.find('\n', line_begin);
        const std::string_view line = text.substr(
            line_begin, (line_end == std::string_view::npos ? text.size() : line_end) - line_begin);
        const bool blank = std::all_of(line.begin(), line.end(), [](char character) {
            return character == ' ' || character == '\t' || character == '\r';
        });
        if (!blank && previous_line_blank) ++paragraphs;
        paragraph_has_text = paragraph_has_text || !blank;
        previous_line_blank = blank;
        if (line_end == std::string_view::npos) break;
        line_begin = line_end + 1U;
    }
    return paragraph_has_text ? std::max<std::size_t>(paragraphs, 1U) : 0U;
}

[[nodiscard]] double bounded(double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] std::string issue_summary(const std::vector<std::string>& issues) {
    if (issues.empty()) return "nenhum problema estrutural detectado";
    std::ostringstream output;
    for (std::size_t index = 0U; index < issues.size(); ++index) {
        if (index != 0U) output << "; ";
        output << issues[index];
    }
    return output.str();
}

} // namespace

PowerMode parse_power_mode(std::string_view value) {
    std::string normalized;
    for (const std::string& word : normalized_words(value)) {
        if (!normalized.empty()) normalized.push_back(' ');
        normalized += word;
    }
    if (normalized.empty() || normalized == "medium" || normalized == "medio"
        || normalized == "media" || normalized == "equilibrado") {
        return PowerMode::medium;
    }
    if (normalized == "flash" || normalized == "rapido" || normalized == "economico") {
        return PowerMode::flash;
    }
    if (normalized == "high" || normalized == "alto" || normalized == "alta"
        || normalized == "alta potencia" || normalized == "potente") {
        return PowerMode::high;
    }
    throw Error("modo de potência inválido; use flash, medium ou high");
}

std::string_view power_mode_name(PowerMode mode) noexcept {
    switch (mode) {
    case PowerMode::flash: return "flash";
    case PowerMode::medium: return "medium";
    case PowerMode::high: return "high";
    }
    return "medium";
}

const PowerProfile& power_profile(PowerMode mode) noexcept {
    static constexpr PowerProfile flash{
        PowerMode::flash,
        "flash",
        "Flash",
        "Responda de forma concisa, priorizando a conclusão e no máximo os detalhes essenciais.",
        256U, 3U, 512U, 8U, 0.3F, 1.12F, false};
    static constexpr PowerProfile medium{
        PowerMode::medium,
        "medium",
        "Medium",
        "Responda com estrutura, exemplos úteis e explicação suficiente para sustentar a conclusão.",
        1'024U, 6U, 2'048U, 20U, 0.5F, 1.10F, false};
    static constexpr PowerProfile high{
        PowerMode::high,
        "high",
        "Alta potência",
        "Produza uma resposta aprofundada e bem estruturada; explicite premissas, verifique "
        "contradições e inclua exemplos quando melhorarem a precisão.",
        4'096U, 12U, 4'096U, 40U, 0.55F, 1.08F, true};
    switch (mode) {
    case PowerMode::flash: return flash;
    case PowerMode::medium: return medium;
    case PowerMode::high: return high;
    }
    return medium;
}

std::vector<std::string> significant_words(std::string_view text) {
    std::string expanded;
    expanded.reserve(text.size() + 16U);
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if ((text[index] == 'C' || text[index] == 'c') && index + 2U < text.size()
            && text[index + 1U] == '+' && text[index + 2U] == '+') {
            expanded += " cpp ";
            index += 2U;
        } else if ((text[index] == 'C' || text[index] == 'c') && index + 1U < text.size()
                   && text[index + 1U] == '#') {
            expanded += " csharp ";
            ++index;
        } else {
            expanded.push_back(text[index]);
        }
    }
    static const std::set<std::string, std::less<>> stop_words{
        "a", "ao", "aos", "as", "com", "como", "da", "das", "de", "do", "dos",
        "e", "em", "essa", "esse", "esta", "este", "eu", "foi", "me", "meu", "minha",
        "na", "nas", "no", "nos", "o", "os", "ou", "para", "por", "qual", "que",
        "quem", "se", "ser", "seu", "sua", "sobre", "um", "uma", "voce", "the",
        "what", "how", "is", "are", "of", "to", "and", "in"
    };
    std::set<std::string> unique;
    for (std::string word : normalized_words(expanded)) {
        if (word.size() >= 2U && !stop_words.contains(word)) unique.insert(std::move(word));
    }
    return {unique.begin(), unique.end()};
}

TextMetrics analyze_text(std::string_view text) {
    TextMetrics metrics;
    metrics.bytes = text.size();
    const std::vector<std::string> words = normalized_words(text);
    metrics.words = words.size();
    std::set<std::string> unique(words.begin(), words.end());
    metrics.unique_words = unique.size();
    metrics.paragraphs = paragraph_count(text);

    // Frases são segmentadas por pontuação explícita. Guardar seus tamanhos permite
    // medir legibilidade sem depender de uma biblioteca linguística opaca.
    std::vector<std::size_t> sentence_lengths;
    std::size_t sentence_words = 0U;
    bool inside_word = false;
    std::size_t offset = 0U;
    while (offset < text.size()) {
        const std::uint32_t codepoint = next_codepoint(text, offset);
        ++metrics.characters;
        const bool word_character = fold_latin(codepoint) != '\0';
        if (word_character && !inside_word) ++sentence_words;
        inside_word = word_character;
        if (codepoint == static_cast<std::uint32_t>('.')
            || codepoint == static_cast<std::uint32_t>('!')
            || codepoint == static_cast<std::uint32_t>('?')) {
            if (sentence_words != 0U) {
                sentence_lengths.push_back(sentence_words);
                metrics.longest_sentence_words = std::max(
                    metrics.longest_sentence_words, sentence_words);
                sentence_words = 0U;
            }
            inside_word = false;
        }
    }
    if (sentence_words != 0U) {
        sentence_lengths.push_back(sentence_words);
        metrics.longest_sentence_words = std::max(metrics.longest_sentence_words, sentence_words);
    }
    metrics.sentences = sentence_lengths.size();
    if (metrics.words != 0U && metrics.sentences == 0U) metrics.sentences = 1U;
    if (metrics.sentences != 0U) {
        metrics.average_words_per_sentence = static_cast<double>(metrics.words)
                                             / static_cast<double>(metrics.sentences);
    }
    if (metrics.words != 0U) {
        metrics.lexical_diversity = static_cast<double>(metrics.unique_words)
                                    / static_cast<double>(metrics.words);
        metrics.repeated_word_ratio = static_cast<double>(metrics.words - metrics.unique_words)
                                      / static_cast<double>(metrics.words);
    }

    const std::size_t long_sentences = static_cast<std::size_t>(std::count_if(
        sentence_lengths.begin(), sentence_lengths.end(),
        [](std::size_t length) { return length > 30U; }));
    if (!sentence_lengths.empty()) {
        metrics.long_sentence_ratio = static_cast<double>(long_sentences)
                                      / static_cast<double>(sentence_lengths.size());
    }

    // Tópicos são os termos informativos mais frequentes. Empates são ordenados
    // lexicalmente, tornando o relatório reprodutível em qualquer plataforma.
    const std::vector<std::string> significant = significant_words(text);
    std::unordered_map<std::string, std::size_t> frequencies;
    for (const std::string& word : words) {
        if (std::binary_search(significant.begin(), significant.end(), word)) ++frequencies[word];
    }
    std::vector<std::pair<std::string, std::size_t>> ranked;
    ranked.reserve(significant.size());
    for (const std::string& word : significant) {
        const auto found = frequencies.find(word);
        ranked.emplace_back(word, found == frequencies.end() ? 1U : found->second);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return left.second != right.second ? left.second > right.second : left.first < right.first;
    });
    const std::size_t topic_count = std::min<std::size_t>(ranked.size(), 8U);
    metrics.top_terms.reserve(topic_count);
    for (std::size_t index = 0U; index < topic_count; ++index) {
        metrics.top_terms.push_back(std::move(ranked[index].first));
    }

    double average_word_characters = 0.0;
    for (const std::string& word : words) {
        average_word_characters += static_cast<double>(word.size());
    }
    if (!words.empty()) average_word_characters /= static_cast<double>(words.size());
    const double sentence_penalty = bounded(
        (metrics.average_words_per_sentence - 18.0) / 32.0);
    const double word_penalty = bounded((average_word_characters - 8.0) / 8.0);
    metrics.readability_score = metrics.words == 0U ? 0.0
        : bounded(1.0 - 0.45 * sentence_penalty - 0.35 * metrics.long_sentence_ratio
                  - 0.20 * word_penalty);
    metrics.structural_coherence_score = metrics.words == 0U ? 0.0
        : bounded(1.0 - 0.45 * metrics.long_sentence_ratio
                  - 0.40 * metrics.repeated_word_ratio
                  - (metrics.longest_sentence_words > 90U ? 0.20 : 0.0));

    std::size_t code_markers = 0U;
    std::size_t marker = 0U;
    while ((marker = text.find("```", marker)) != std::string_view::npos) {
        ++code_markers;
        marker += 3U;
    }
    metrics.code_blocks = code_markers / 2U;
    return metrics;
}

ResponseAssessment assess_response(std::string_view prompt,
                                   std::string_view response) {
    ResponseAssessment assessment;
    assessment.metrics = analyze_text(response);
    const bool transformed_output = transformation_request(prompt);
    bool fatal = false;
    if (response.size() < 12U
        && !(transformed_output && assessment.metrics.words != 0U)) {
        assessment.issues.emplace_back("resposta curta demais");
        fatal = true;
    }
    if (response.size() > 64U * 1024U) {
        assessment.issues.emplace_back("resposta excede 64 KiB");
        fatal = true;
    }
    if (contains_role_marker(response)) {
        assessment.issues.emplace_back("vazamento de marcador de papel");
        fatal = true;
    }
    if (contains_generic_evasion(response)) {
        assessment.issues.emplace_back("evasão genérica sem responder ao pedido");
        fatal = true;
    }

    const std::vector<std::string> query_words = significant_words(prompt);
    const std::vector<std::string> answer_words = significant_words(response);
    std::size_t overlap = 0U;
    for (const std::string& word : query_words) {
        if (std::binary_search(answer_words.begin(), answer_words.end(), word)) ++overlap;
    }
    if (transformed_output || query_words.empty()) {
        assessment.relevance_score = answer_words.empty() ? 0.0 : 1.0;
    } else {
        assessment.relevance_score = static_cast<double>(overlap)
                                     / static_cast<double>(query_words.size());
    }
    const std::size_t required_overlap = transformed_output ? 0U
                                          : query_words.size() >= 4U ? 2U
                                          : query_words.empty() ? 0U : 1U;
    if (overlap < required_overlap) {
        assessment.issues.emplace_back("baixa sobreposição temática com a pergunta");
        fatal = true;
    }

    double coherence = assessment.metrics.structural_coherence_score;
    if (assessment.metrics.words < 3U) coherence *= 0.4;
    if (assessment.metrics.longest_sentence_words > 90U) {
        assessment.issues.emplace_back("frase excessivamente longa");
        coherence *= 0.70;
    }
    if (assessment.metrics.average_words_per_sentence > 45.0) coherence *= 0.80;
    if (assessment.metrics.words >= 20U && assessment.metrics.lexical_diversity < 0.24) {
        assessment.issues.emplace_back("repetição lexical excessiva");
        coherence *= 0.35;
        fatal = true;
    }
    assessment.coherence_score = bounded(coherence);
    const double lexical_component = assessment.metrics.words == 0U
        ? 0.0 : bounded(assessment.metrics.lexical_diversity / 0.65);
    assessment.quality_score = bounded(0.50 * assessment.relevance_score
                                       + 0.30 * assessment.coherence_score
                                       + 0.20 * lexical_component);
    assessment.accepted = !fatal && assessment.quality_score >= 0.42;
    if (!assessment.accepted && assessment.issues.empty()) {
        assessment.issues.emplace_back("pontuação de qualidade abaixo do mínimo");
    }
    return assessment;
}

std::string format_text_analysis(std::string_view text) {
    const TextMetrics metrics = analyze_text(text);
    std::ostringstream output;
    output << "Análise textual determinística:\n"
           << "- Tamanho: " << metrics.bytes << " bytes / " << metrics.characters
           << " caracteres UTF-8\n"
           << "- Estrutura: " << metrics.paragraphs << " parágrafo(s), "
           << metrics.sentences << " frase(s), " << metrics.words << " palavra(s)\n"
           << "- Vocabulário: " << metrics.unique_words << " palavra(s) distinta(s); diversidade "
           << std::fixed << std::setprecision(1) << metrics.lexical_diversity * 100.0 << "%\n"
           << "- Ritmo: média de " << std::setprecision(1) << metrics.average_words_per_sentence
           << " palavras/frase; maior frase com " << metrics.longest_sentence_words << " palavras\n"
           << "- Legibilidade heurística: " << metrics.readability_score * 100.0
           << "% ; coerência estrutural: " << metrics.structural_coherence_score * 100.0 << "%\n"
           << "- Repetição: " << metrics.repeated_word_ratio * 100.0
           << "% das ocorrências; frases longas: " << metrics.long_sentence_ratio * 100.0 << "%\n"
           << "- Tópicos/termos dominantes: ";
    if (metrics.top_terms.empty()) {
        output << "nenhum";
    } else {
        for (std::size_t index = 0U; index < metrics.top_terms.size(); ++index) {
            if (index != 0U) output << ", ";
            output << metrics.top_terms[index];
        }
    }
    output << "\n- Código: " << metrics.code_blocks
           << " bloco(s) delimitado(s) por crases\n";
    if (metrics.words == 0U) {
        output << "Conclusão: não há texto lexical suficiente para uma análise útil.";
    } else if (metrics.long_sentence_ratio >= 0.5
               || metrics.longest_sentence_words > 90U) {
        output << "Conclusão: a legibilidade pode melhorar dividindo frases longas e explicitando "
                  "as ligações entre ideias.";
    } else if (metrics.words >= 20U
               && (metrics.lexical_diversity < 0.24
                   || metrics.repeated_word_ratio > 0.45)) {
        output << "Conclusão: há repetição lexical alta; revise termos redundantes.";
    } else {
        output << "Conclusão: a estrutura mensurável não apresenta anomalia grave. "
                  "Estas métricas não provam que os fatos estejam corretos.";
    }
    return output.str();
}

std::string format_response_analysis(std::string_view prompt,
                                     std::string_view response) {
    const ResponseAssessment assessment = assess_response(prompt, response);
    std::ostringstream output;
    output << "Autoanálise da resposta anterior:\n"
           << "- Qualidade estrutural: " << std::fixed << std::setprecision(1)
           << assessment.quality_score * 100.0 << "%\n"
           << "- Relevância lexical: " << assessment.relevance_score * 100.0 << "%\n"
           << "- Coerência mensurável: " << assessment.coherence_score * 100.0 << "%\n"
           << "- Extensão: " << assessment.metrics.words << " palavras em "
           << assessment.metrics.sentences << " frases\n"
           << "- Filtro: " << (assessment.accepted ? "aprovada" : "reprovada") << "\n"
           << "- Observações: " << issue_summary(assessment.issues) << "\n"
           << "Limite: esta autoanálise mede estrutura, repetição e aderência lexical; ela não "
              "transforma opinião em fato nem substitui testes ou fontes externas.";
    return output.str();
}

void BehaviorMonitor::record(const ResponseAssessment& assessment, bool revised) noexcept {
    response_count_.fetch_add(1U, std::memory_order_relaxed);
    if (!assessment.accepted) rejected_count_.fetch_add(1U, std::memory_order_relaxed);
    if (revised) revised_count_.fetch_add(1U, std::memory_order_relaxed);
    const double bounded_quality = bounded(assessment.quality_score);
    const auto micros = static_cast<std::uint64_t>(std::llround(bounded_quality * 1'000'000.0));
    quality_micros_.fetch_add(micros, std::memory_order_relaxed);
}

BehaviorSnapshot BehaviorMonitor::snapshot() const noexcept {
    BehaviorSnapshot snapshot;
    snapshot.response_count = response_count_.load(std::memory_order_relaxed);
    snapshot.rejected_count = rejected_count_.load(std::memory_order_relaxed);
    snapshot.revised_count = revised_count_.load(std::memory_order_relaxed);
    const std::uint64_t quality = quality_micros_.load(std::memory_order_relaxed);
    if (snapshot.response_count != 0U) {
        snapshot.average_quality = static_cast<double>(quality)
                                   / (static_cast<double>(snapshot.response_count) * 1'000'000.0);
    }
    return snapshot;
}

} // namespace cortex
