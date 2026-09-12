#include "cortex/native_cognition.hpp"

#include "cortex/core.hpp"
#include "cortex/knowledge.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <iomanip>
#include <map>
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
    case 0x00C7U: case 0x00E7U: return 'c';
    case 0x00C8U: case 0x00C9U: case 0x00CAU: case 0x00CBU:
    case 0x00E8U: case 0x00E9U: case 0x00EAU: case 0x00EBU:
        return 'e';
    case 0x00CCU: case 0x00CDU: case 0x00CEU: case 0x00CFU:
    case 0x00ECU: case 0x00EDU: case 0x00EEU: case 0x00EFU:
        return 'i';
    case 0x00D1U: case 0x00F1U: return 'n';
    case 0x00D2U: case 0x00D3U: case 0x00D4U: case 0x00D5U: case 0x00D6U:
    case 0x00F2U: case 0x00F3U: case 0x00F4U: case 0x00F5U: case 0x00F6U:
        return 'o';
    case 0x00D9U: case 0x00DAU: case 0x00DBU: case 0x00DCU:
    case 0x00F9U: case 0x00FAU: case 0x00FBU: case 0x00FCU:
        return 'u';
    default: return '\0';
    }
}

[[nodiscard]] std::uint32_t next_codepoint(std::string_view text,
                                           std::size_t& offset) noexcept {
    const unsigned char first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80U) return first;
    std::uint32_t value = 0U;
    std::size_t remaining = 0U;
    if ((first & 0xE0U) == 0xC0U) {
        value = static_cast<std::uint32_t>(first & 0x1FU);
        remaining = 1U;
    } else if ((first & 0xF0U) == 0xE0U) {
        value = static_cast<std::uint32_t>(first & 0x0FU);
        remaining = 2U;
    } else if ((first & 0xF8U) == 0xF0U) {
        value = static_cast<std::uint32_t>(first & 0x07U);
        remaining = 3U;
    } else {
        return 0xFFFDU;
    }
    if (offset + remaining > text.size()) {
        offset = text.size();
        return 0xFFFDU;
    }
    for (std::size_t index = 0U; index < remaining; ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[offset]);
        if ((byte & 0xC0U) != 0x80U) return 0xFFFDU;
        ++offset;
        value = (value << 6U) | static_cast<std::uint32_t>(byte & 0x3FU);
    }
    return value;
}

[[nodiscard]] std::string canonical_text(std::string_view text) {
    std::string output;
    output.reserve(text.size());
    bool pending_space = false;
    std::size_t offset = 0U;
    while (offset < text.size()) {
        const char folded = fold_latin(next_codepoint(text, offset));
        if (folded != '\0') {
            if (pending_space && !output.empty()) output.push_back(' ');
            output.push_back(folded);
            pending_space = false;
        } else {
            pending_space = !output.empty();
        }
    }
    return output;
}

[[nodiscard]] std::string lowercase_ascii_copy(std::string_view text) {
    std::string output(text);
    for (char& character : output) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return output;
}

[[nodiscard]] bool phrase(std::string_view canonical, std::string_view expected) {
    if (canonical == expected) return true;
    std::string padded;
    padded.reserve(canonical.size() + 2U);
    padded.push_back(' ');
    padded.append(canonical);
    padded.push_back(' ');
    return padded.find(" " + std::string(expected) + " ") != std::string::npos;
}

[[nodiscard]] bool any_phrase(std::string_view canonical,
                              std::initializer_list<std::string_view> choices) {
    return std::any_of(choices.begin(), choices.end(), [&](std::string_view choice) {
        return phrase(canonical, choice);
    });
}

[[nodiscard]] std::string trim_copy(std::string_view value) {
    const auto whitespace = [](char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    };
    while (!value.empty() && whitespace(value.front())) value.remove_prefix(1U);
    while (!value.empty() && whitespace(value.back())) value.remove_suffix(1U);
    return std::string(value);
}

void truncate_utf8(std::string& text, std::size_t maximum_bytes) {
    if (text.size() <= maximum_bytes) return;
    std::size_t boundary = maximum_bytes;
    while (boundary > 0U
           && (static_cast<unsigned char>(text[boundary]) & 0xC0U) == 0x80U) {
        --boundary;
    }
    text.resize(boundary);
    text += "\n\n[Resposta limitada pelo perfil de potência.]";
}

[[nodiscard]] std::uint64_t fnv1a(std::string_view value,
                                  std::uint64_t seed = 14'695'981'039'346'656'037ULL) noexcept {
    std::uint64_t hash = seed;
    for (const unsigned char byte : value) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

[[nodiscard]] std::vector<std::string_view> split_views(std::string_view canonical) {
    std::vector<std::string_view> words;
    std::size_t begin = 0U;
    while (begin < canonical.size()) {
        const std::size_t end = canonical.find(' ', begin);
        const std::size_t actual_end = end == std::string_view::npos ? canonical.size() : end;
        if (actual_end > begin) words.push_back(canonical.substr(begin, actual_end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return words;
}

[[nodiscard]] std::string topic_from_prompt(std::string_view prompt) {
    std::string topic = trim_copy(prompt);
    const std::size_t colon = topic.find(':');
    if (colon != std::string::npos && colon + 1U < topic.size()) {
        topic = trim_copy(std::string_view(topic).substr(colon + 1U));
    } else {
        // "sobre" delimita o tema sem perder acentos, caixa ou ordem das palavras originais.
        const std::string lowered = lowercase_ascii_copy(topic);
        const std::size_t about = lowered.find(" sobre ");
        if (about != std::string::npos && about + 7U < topic.size()) {
            topic = trim_copy(std::string_view(topic).substr(about + 7U));
        } else {
            constexpr std::array<std::string_view, 13U> prefixes{
                "como implementar", "como criar", "me ensine", "compare", "explique",
                "implemente", "refatore", "planeje", "analise", "crie", "escreva",
                "redija", "sintetize"
            };
            const std::string canonical = canonical_text(topic);
            for (const std::string_view prefix : prefixes) {
                if (!canonical.starts_with(prefix)) continue;
                const std::size_t words_to_remove = static_cast<std::size_t>(
                    std::count(prefix.begin(), prefix.end(), ' ')) + 1U;
                std::size_t offset = 0U;
                std::size_t removed = 0U;
                while (offset < topic.size() && removed < words_to_remove) {
                    while (offset < topic.size()
                           && std::isspace(static_cast<unsigned char>(topic[offset])) != 0) ++offset;
                    while (offset < topic.size()
                           && std::isspace(static_cast<unsigned char>(topic[offset])) == 0) ++offset;
                    ++removed;
                }
                topic = trim_copy(std::string_view(topic).substr(offset));
                break;
            }
        }
    }
    while (!topic.empty() && (topic.back() == '.' || topic.back() == '?' || topic.back() == '!')) {
        topic.pop_back();
    }
    topic = trim_copy(topic);
    if (topic.size() > 180U) {
        topic.resize(180U);
        while (!topic.empty()
               && (static_cast<unsigned char>(topic.back()) & 0xC0U) == 0x80U) {
            topic.pop_back();
        }
        topic += "…";
    }
    return topic.empty() ? std::string("o tema solicitado") : topic;
}

[[nodiscard]] NativeCognitiveIntent detect_intent(std::string_view prompt) {
    const std::string canonical = canonical_text(prompt);
    if (any_phrase(canonical, {"codigo", "programacao", "compile", "compilador", "funcao",
                               "classe", "algoritmo", "cpp", "c++", "rust", "golang", "zig",
                               "debug", "bug", "refatore", "implemente"})) {
        return NativeCognitiveIntent::programming;
    }
    if (any_phrase(canonical, {"resuma", "resumir", "resumo", "sintetize", "sumarize"})) {
        return NativeCognitiveIntent::summarization;
    }
    if (any_phrase(canonical, {"artigo", "ensaio", "texto longo", "redija", "redacao",
                               "relatorio detalhado", "escrita longa"})) {
        return NativeCognitiveIntent::long_form_writing;
    }
    if (any_phrase(canonical, {"historia", "conto", "poema", "poesia", "criativo",
                               "criativa", "narrativa", "personagem"})) {
        return NativeCognitiveIntent::creative_writing;
    }
    if (any_phrase(canonical, {"ideias", "brainstorm", "alternativas", "sugestoes"})) {
        return NativeCognitiveIntent::brainstorming;
    }
    if (any_phrase(canonical, {"compare", "comparacao", "diferenca", "versus", "melhor que"})) {
        return NativeCognitiveIntent::comparison;
    }
    if (any_phrase(canonical, {"decidir", "escolher", "decisao", "vale a pena", "qual opcao"})) {
        return NativeCognitiveIntent::decision;
    }
    if (any_phrase(canonical, {"passo a passo", "tutorial", "como fazer", "como criar",
                               "como instalar", "procedimento"})) {
        return NativeCognitiveIntent::procedure;
    }
    if (any_phrase(canonical, {"raciocine", "raciocinio", "investigue", "diagnostique",
                               "resolva", "planeje", "plano", "hipotese", "problema complexo"})) {
        return NativeCognitiveIntent::reasoning;
    }
    if (any_phrase(canonical, {"oi", "ola", "bom dia", "boa tarde", "boa noite",
                               "obrigado", "obrigada", "tudo bem", "como voce esta",
                               "vamos conversar", "converse comigo", "o que voce acha",
                               "qual sua opiniao", "estou pensando"})) {
        return NativeCognitiveIntent::conversation;
    }
    return NativeCognitiveIntent::explanation;
}

struct Evidence final {
    std::string title;
    std::string category;
    std::string text;
    std::string source;
    double score = 0.0;
    bool memory = false;
};

[[nodiscard]] std::string expanded_query(std::string_view prompt) {
    std::string result(prompt);
    // Expansões ajudam consultas vagas, mas em pedidos específicos acrescentariam temas
    // laterais (por exemplo, "dados" -> visualização/banco) e reduziriam a precisão.
    if (significant_words(prompt).size() > 4U) return result;
    const std::string canonical = canonical_text(prompt);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 18U> expansions{{
        {"software", " programação arquitetura testes"},
        {"programa", " programação algoritmo código"},
        {"computador", " computação sistema operacional hardware"},
        {"site", " web HTTP acessibilidade segurança"},
        {"banco", " banco de dados SQL transação índice"},
        {"inteligencia artificial", " IA aprendizado modelo neural"},
        {"seguro", " segurança validação ameaça"},
        {"rapido", " desempenho otimização benchmark"},
        {"internet", " rede HTTP TLS DNS"},
        {"dinheiro", " economia inflação juros investimento"},
        {"universo", " astronomia sistema solar"},
        {"vida", " biologia evolução saúde"},
        {"argumento", " lógica premissa conclusão evidência"},
        {"dados", " estatística visualização banco privacidade"},
        {"texto", " escrita legibilidade coerência análise"},
        {"cortex", " Cortex arquitetura autoevolução memória"},
        {"roblox", " Roblox diagnóstico launcher Windows"},
        {"windows", " sistema operacional processo arquivo segurança"}
    }};
    for (const auto& [trigger, addition] : expansions) {
        if (phrase(canonical, trigger)) result += addition;
    }
    return result;
}

[[nodiscard]] std::string memory_payload(std::string_view memory) {
    constexpr std::array<std::string_view, 2U> content_markers{"\nConteúdo: ", "\nConteudo: "};
    for (const std::string_view marker : content_markers) {
        const std::size_t found = memory.find(marker);
        if (found != std::string_view::npos) {
            return trim_copy(memory.substr(found + marker.size()));
        }
    }
    return trim_copy(memory);
}

[[nodiscard]] std::vector<Evidence> gather_evidence(
    std::string_view prompt,
    std::span<const MemoryHit> memories,
    std::span<const CuratedKnowledgeAnswer> supplied,
    std::size_t maximum) {
    std::vector<Evidence> evidence;
    if (maximum == 0U) return evidence;
    for (const CuratedKnowledgeAnswer& answer : supplied) {
        if (answer.text.empty()) continue;
        evidence.push_back(Evidence{
            answer.title, answer.category, answer.text, answer.category,
            std::clamp(answer.score, 0.0, 1.0), false});
    }
    const std::vector<CuratedKnowledgeAnswer> curated = search_curated_knowledge(
        expanded_query(prompt), maximum, 0.10);
    for (const CuratedKnowledgeAnswer& answer : curated) {
        evidence.push_back(Evidence{
            answer.title, answer.category, answer.text, "base curada", answer.score, false});
    }

    for (const MemoryHit& memory : memories) {
        const std::string_view stored(memory.text);
        const bool web = stored.starts_with("[WEB]");
        const bool document = stored.starts_with("[DOC]");
        if (!web && !document) continue;
        const std::string payload = memory_payload(stored);
        if (payload.empty()) continue;
        const std::string source = web ? "pesquisa web memorizada" : "documento local";
        const double score = std::clamp(static_cast<double>(memory.score), 0.0, 1.0);
        evidence.push_back(Evidence{
            web ? "Conhecimento pesquisado" : "Trecho recuperado", source, payload,
            source, score, true});
    }
    std::stable_sort(evidence.begin(), evidence.end(), [](const Evidence& left,
                                                           const Evidence& right) {
        if (left.score != right.score) return left.score > right.score;
        return left.memory && !right.memory;
    });

    // Evita repetir a mesma entrada quando a memória contém uma conversa sobre a base curada.
    std::set<std::string> seen;
    std::vector<Evidence> unique;
    unique.reserve(std::min(maximum, evidence.size()));
    for (Evidence& item : evidence) {
        const std::string key = canonical_text(item.title + " " + item.text.substr(0U, 120U));
        if (!seen.insert(key).second) continue;
        unique.push_back(std::move(item));
        if (unique.size() == maximum) break;
    }
    return unique;
}

[[nodiscard]] std::vector<std::string> split_sentences(std::string_view text) {
    std::vector<std::string> sentences;
    std::size_t begin = 0U;
    for (std::size_t index = 0U; index < text.size(); ++index) {
        const char character = text[index];
        const bool boundary = character == '.' || character == '!' || character == '?'
                              || character == '\n';
        if (!boundary) continue;
        std::size_t end = index + (character == '\n' ? 0U : 1U);
        std::string sentence = trim_copy(text.substr(begin, end - begin));
        if (!sentence.empty()) sentences.push_back(std::move(sentence));
        begin = index + 1U;
    }
    if (begin < text.size()) {
        std::string sentence = trim_copy(text.substr(begin));
        if (!sentence.empty()) sentences.push_back(std::move(sentence));
    }
    return sentences;
}

[[nodiscard]] std::string summarize_extractively(std::string_view prompt,
                                                  PowerMode power_mode) {
    std::string_view source = prompt;
    const std::size_t colon = source.find(':');
    if (colon != std::string_view::npos) source.remove_prefix(colon + 1U);
    const std::size_t newline = source.find('\n');
    if (colon == std::string_view::npos && newline != std::string_view::npos) {
        source.remove_prefix(newline + 1U);
    }
    source = std::string_view(source.data(), source.size());
    const std::vector<std::string> sentences = split_sentences(source);
    if (sentences.size() < 2U || trim_copy(source).size() < 80U) return {};

    std::unordered_map<std::string, std::size_t> frequency;
    for (const std::string& sentence : sentences) {
        for (const std::string& word : significant_words(sentence)) ++frequency[word];
    }
    struct Scored final { std::size_t index; double score; };
    std::vector<Scored> scored;
    scored.reserve(sentences.size());
    for (std::size_t index = 0U; index < sentences.size(); ++index) {
        const std::vector<std::string> words = significant_words(sentences[index]);
        double score = 0.0;
        for (const std::string& word : words) {
            score += 1.0 + std::log1p(static_cast<double>(frequency[word]));
        }
        if (!words.empty()) score /= std::sqrt(static_cast<double>(words.size()));
        if (index == 0U) score *= 1.12;
        scored.push_back(Scored{index, score});
    }
    const std::size_t divisor = power_mode == PowerMode::flash ? 5U
                                : power_mode == PowerMode::medium ? 3U : 2U;
    const std::size_t wanted = std::clamp(sentences.size() / divisor,
                                          std::size_t{1U}, std::size_t{8U});
    std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(wanted),
                      scored.end(), [](const Scored& left, const Scored& right) {
        return left.score > right.score;
    });
    scored.resize(wanted);
    std::sort(scored.begin(), scored.end(), [](const Scored& left, const Scored& right) {
        return left.index < right.index;
    });
    std::ostringstream output;
    output << "Resumo:\n";
    for (const Scored& item : scored) output << "- " << sentences[item.index] << '\n';
    output << "\nO resumo é extrativo: preserva afirmações do texto recebido e não confirma "
              "automaticamente se elas são verdadeiras.";
    return output.str();
}

[[nodiscard]] std::string conversation_answer(std::string_view prompt,
                                              std::string_view context) {
    const std::string canonical = canonical_text(prompt);
    if (any_phrase(canonical, {"obrigado", "obrigada", "valeu"})) {
        return "Por nada. Posso continuar desse ponto, comparar alternativas ou transformar a ideia "
               "em um plano verificável.";
    }
    if (any_phrase(canonical, {"tudo bem", "como voce esta"})) {
        return "Estou funcionando normalmente. Não tenho sentimentos, mas consigo acompanhar a "
               "conversa, recuperar memórias e trabalhar no problema que você escolher.";
    }
    if (any_phrase(canonical, {"bom dia", "boa tarde", "boa noite"})) {
        return "Olá. Podemos conversar livremente, estudar um assunto, planejar um projeto, analisar "
               "texto ou construir e testar código.";
    }
    if (any_phrase(canonical, {"oi", "ola", "vamos conversar"})) {
        if (!context.empty()) {
            return "Olá novamente. Tenho o contexto recente desta conversa; diga se quer continuar o "
                   "assunto anterior ou começar outro.";
        }
        return "Olá! Escolha qualquer assunto. Se a pergunta depender de informação atual, posso "
               "pesquisar e mostrar a fonte em vez de inventar.";
    }
    if (any_phrase(canonical, {"converse comigo", "o que voce acha", "qual sua opiniao",
                               "estou pensando"})) {
        const std::string topic = topic_from_prompt(prompt);
        return "Posso conversar sobre " + topic
               + ". Não tenho opinião ou experiência pessoal, mas posso explorar a ideia por três "
                 "ângulos: o que ela tenta melhorar, quais custos ou riscos pode esconder e que "
                 "evidência mudaria nossa conclusão. Qual desses ângulos você quer desenvolver primeiro?";
    }
    return {};
}

[[nodiscard]] std::string creative_answer(std::string_view prompt, PowerMode power_mode) {
    const std::string canonical = canonical_text(prompt);
    const std::string topic = topic_from_prompt(prompt);
    const std::uint64_t selector = fnv1a(canonical);
    if (any_phrase(canonical, {"poema", "poesia"})) {
        constexpr std::array<std::string_view, 6U> images{
            "uma luz que atravessa a janela", "o rumor paciente da chuva",
            "um mapa dobrado no bolso", "a engrenagem secreta do tempo",
            "um horizonte aceso de perguntas", "o silêncio antes da descoberta"
        };
        const std::string_view image = images[selector % images.size()];
        std::ostringstream poem;
        poem << "**Entre perguntas e caminhos**\n\n"
             << "Sobre " << topic << ", nasce um traço,\n"
             << image << ",\n"
             << "uma ideia pequena buscando espaço,\n"
             << "um passo que aprende a ser direção.\n\n"
             << "Não basta correr: é preciso escutar\n"
             << "o erro, a dúvida, o dado e o sinal;\n"
             << "cada resposta convida a testar,\n"
             << "cada limite redesenha o final.\n";
        if (power_mode != PowerMode::flash) {
            poem << "\nE quando a noite fecha o caderno,\n"
                 << "permanece uma pergunta em construção:\n"
                 << "o futuro não chega pronto ou eterno —\n"
                 << "ele começa na próxima decisão.\n";
        }
        return poem.str();
    }

    constexpr std::array<std::string_view, 6U> protagonists{
        "Lia", "Caio", "Maya", "Davi", "Nina", "Tomás"
    };
    constexpr std::array<std::string_view, 6U> places{
        "uma cidade onde os relógios guardavam segredos",
        "uma oficina iluminada apenas por monitores antigos",
        "uma estação construída entre a mata e as estrelas",
        "uma biblioteca que mudava de corredores toda noite",
        "um bairro em que cada janela mostrava um futuro diferente",
        "um laboratório esquecido sob uma escola"
    };
    const std::string_view protagonist = protagonists[selector % protagonists.size()];
    const std::string_view place = places[(selector >> 8U) % places.size()];
    std::ostringstream story;
    story << "**O mapa do que ainda não existia**\n\n"
          << protagonist << " vivia em " << place << ". O assunto que ocupava seus pensamentos era "
          << topic << ", não como uma resposta pronta, mas como uma porta que ninguém conseguira abrir.\n\n"
          << "Certa manhã, encontrou um caderno com três páginas. Na primeira havia observações; na "
             "segunda, perguntas; a terceira estava vazia. Em vez de procurar uma solução perfeita, "
             "decidiu registrar o que podia medir e separar certeza de imaginação.\n\n"
          << "A primeira tentativa falhou depressa. O fracasso, porém, revelou uma regra escondida: "
             "cada avanço exigia preservar o que funcionava e mudar somente uma hipótese por vez. "
          << protagonist << " transformou o erro em mapa e o mapa em experiência.\n";
    if (power_mode != PowerMode::flash) {
        story << "\nOutras pessoas chegaram com respostas incompatíveis. Em vez de escolher a voz mais "
                 "confiante, o grupo construiu um teste que todas aceitariam. O resultado contrariou "
                 "duas certezas e uniu três ideias que antes pareciam rivais.\n\n"
              << "Quando a porta enfim se abriu, não havia um tesouro atrás dela, mas uma sala cheia "
                 "de ferramentas. " << protagonist << " compreendeu que a descoberta não encerrava "
                 "o problema: aumentava a capacidade de formular o próximo.\n";
    }
    if (power_mode == PowerMode::high) {
        story << "\nAnos depois, perguntaram qual fora o momento decisivo. " << protagonist
              << " não falou da porta aberta. Falou da página vazia — o lugar onde teve coragem de "
                 "admitir que ainda não sabia e, justamente por isso, começou a aprender.\n";
    }
    return story.str();
}

[[nodiscard]] std::string brainstorm_answer(std::string_view prompt, PowerMode power_mode) {
    const std::string topic = topic_from_prompt(prompt);
    const std::size_t count = power_mode == PowerMode::flash ? 5U
                              : power_mode == PowerMode::medium ? 8U : 12U;
    constexpr std::array<std::pair<std::string_view, std::string_view>, 12U> lenses{{
        {"Versão mínima", "reduza a ideia à menor experiência que produza evidência útil"},
        {"Automação", "elimine uma etapa repetitiva sem esconder decisões importantes"},
        {"Acessibilidade", "adapte a solução para pessoas, equipamentos ou conexões limitadas"},
        {"Verificação", "inclua uma métrica e um teste capazes de provar progresso"},
        {"Modo offline", "preserve uma função central mesmo sem internet"},
        {"Colaboração", "permita que várias pessoas contribuam sem perder autoria ou histórico"},
        {"Aprendizado", "transforme erros frequentes em exemplos e recomendações reutilizáveis"},
        {"Desempenho", "meça o gargalo e projete uma alternativa que use menos memória ou tempo"},
        {"Segurança", "faça permissões explícitas e forneça rollback para ações destrutivas"},
        {"Personalização", "deixe preferências alterarem apresentação sem corromper o núcleo"},
        {"Transparência", "mostre origem, confiança e limites de cada resultado"},
        {"Experimento", "compare duas abordagens mudando apenas uma variável por rodada"}
    }};
    std::ostringstream output;
    output << "Ideias para **" << topic << "**:\n";
    for (std::size_t index = 0U; index < count; ++index) {
        output << index + 1U << ". **" << lenses[index].first << ":** "
               << lenses[index].second << ".\n";
    }
    output << "\nPara escolher, pontue cada ideia de 0 a 5 em impacto, esforço, risco e facilidade "
              "de teste; comece pela maior razão impacto/esforço.";
    return output.str();
}

[[nodiscard]] std::string cpp_program_for(std::string_view canonical) {
    if (any_phrase(canonical, {"fatorial"})) {
        return R"CPP(#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>

std::optional<std::uint64_t> factorial(unsigned int n) {
    std::uint64_t result = 1U;
    for (unsigned int value = 2U; value <= n; ++value) {
        if (result > std::numeric_limits<std::uint64_t>::max() / value) return std::nullopt;
        result *= value;
    }
    return result;
}

int main() {
    unsigned int n = 0U;
    if (!(std::cin >> n)) return 1;
    const auto result = factorial(n);
    if (!result) {
        std::cerr << "overflow\n";
        return 2;
    }
    std::cout << *result << '\n';
}
)CPP";
    }
    if (any_phrase(canonical, {"fibonacci"})) {
        return R"CPP(#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>

std::optional<std::uint64_t> fibonacci(unsigned int n) {
    std::uint64_t previous = 0U;
    std::uint64_t current = 1U;
    for (unsigned int index = 0U; index < n; ++index) {
        if (current > std::numeric_limits<std::uint64_t>::max() - previous) return std::nullopt;
        const std::uint64_t next = previous + current;
        previous = current;
        current = next;
    }
    return previous;
}

int main() {
    unsigned int n = 0U;
    if (!(std::cin >> n)) return 1;
    const auto result = fibonacci(n);
    if (!result) return 2;
    std::cout << *result << '\n';
}
)CPP";
    }
    if (any_phrase(canonical, {"palindromo", "palindrome"})) {
        return R"CPP(#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

bool is_palindrome(const std::string& text) {
    std::string normalized;
    for (const unsigned char character : text) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return std::equal(normalized.begin(), normalized.begin() + normalized.size() / 2U,
                      normalized.rbegin());
}

int main() {
    std::string line;
    std::getline(std::cin, line);
    std::cout << (is_palindrome(line) ? "sim" : "não") << '\n';
}
)CPP";
    }
    if (any_phrase(canonical, {"numero primo", "numeros primos", "primalidade"})) {
        return R"CPP(#include <cstdint>
#include <iostream>

bool is_prime(std::uint64_t value) {
    if (value < 2U) return false;
    if (value % 2U == 0U) return value == 2U;
    for (std::uint64_t divisor = 3U; divisor <= value / divisor; divisor += 2U) {
        if (value % divisor == 0U) return false;
    }
    return true;
}

int main() {
    std::uint64_t value = 0U;
    if (!(std::cin >> value)) return 1;
    std::cout << (is_prime(value) ? "primo" : "não primo") << '\n';
}
)CPP";
    }
    if (any_phrase(canonical, {"busca binaria", "binary search"})) {
        return R"CPP(#include <algorithm>
#include <iostream>
#include <vector>

int main() {
    std::vector<int> values{1, 4, 7, 9, 15, 22};
    const int wanted = 9;
    const auto found = std::lower_bound(values.begin(), values.end(), wanted);
    if (found != values.end() && *found == wanted) {
        std::cout << "índice " << std::distance(values.begin(), found) << '\n';
    } else {
        std::cout << "não encontrado\n";
    }
}
)CPP";
    }
    if (any_phrase(canonical, {"ler arquivo", "leitura arquivo"})) {
        return R"CPP(#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

std::string read_text_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("não foi possível abrir o arquivo");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    try {
        std::cout << read_text_file(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
)CPP";
    }
    return {};
}

[[nodiscard]] std::string programming_answer(std::string_view prompt,
                                             const std::vector<Evidence>& evidence) {
    const std::string canonical = canonical_text(prompt);
    const bool asks_implementation = any_phrase(canonical, {
        "crie", "escreva", "implemente", "codigo para", "exemplo de codigo"
    });
    if (asks_implementation
        && !any_phrase(canonical, {"rust", "golang", "go", "zig"})) {
        const std::string program = cpp_program_for(canonical);
        if (!program.empty()) {
            std::ostringstream output;
            output << "Uma implementação C++20 completa e verificável:\n\n```cpp\n"
                   << program << "```\n"
                   << "Compile com `g++ -std=c++20 -Wall -Wextra -Wpedantic arquivo.cpp`. "
                      "Os limites e erros de entrada são tratados explicitamente.";
            return output.str();
        }
    }

    if (!evidence.empty()) {
        std::ostringstream output;
        output << "**Análise de engenharia**\n\n" << evidence.front().text << "\n\n"
               << "**Plano de implementação**\n"
               << "1. Defina tipos de entrada, saída, invariantes e erros observáveis.\n"
               << "2. Separe transformação pura de arquivos, rede, relógio e estado compartilhado.\n"
               << "3. Implemente primeiro o caminho mínimo correto e mantenha ownership explícito.\n"
               << "4. Cubra caso nominal, fronteiras, entrada inválida e falha de dependência.\n"
               << "5. Compile com warnings e execute sanitizers antes de otimizar.\n";
        if (evidence.size() > 1U) {
            output << "\n**Conhecimento relacionado — " << evidence[1].title << "**\n"
                   << evidence[1].text << '\n';
        }
        output << "\nPara gerar um arquivo completo sem adivinhar contratos, informe linguagem, "
                  "formato da entrada, saída esperada e restrições de desempenho.";
        return output.str();
    }

    if (asks_implementation) {
        return "Consigo construir essa implementação, mas faltam contratos que não devo inventar. "
               "Informe a linguagem, um exemplo de entrada e saída, limites de tamanho e plataforma. "
               "Com isso, preparo código completo, testes de fronteira e comando de compilação.";
    }
    return {};
}

[[nodiscard]] std::string reasoning_answer(std::string_view prompt,
                                           const std::vector<Evidence>& evidence) {
    const std::string canonical = canonical_text(prompt);
    const bool server_under_load = any_phrase(canonical, {"servidor", "servico", "api"})
        && any_phrase(canonical, {"carga", "latencia", "lento", "throughput", "p95", "p99"});
    if (server_under_load) {
        std::ostringstream diagnosis;
        diagnosis << "**Objetivo mensurável**\n"
                  << "Reproduza a lentidão com a mesma versão e dados, registrando taxa de requisições, "
                     "p50/p95/p99, erros e duração. Defina antes o limiar que caracteriza a regressão.\n\n"
                  << "**Hipóteses concorrentes e sinais que as distinguem**\n"
                  << "1. **CPU saturada:** uso por núcleo próximo do limite, fila executável e perfil "
                     "concentrado em poucas funções.\n"
                  << "2. **Memória/paginação:** working set crescente, page faults e acesso ao disco; "
                     "no X40 com HD, swap pode multiplicar a latência.\n"
                  << "3. **Fila ou pool esgotado:** tempo de espera cresce antes do processamento; "
                     "meça fila HTTP, workers, conexões e pool do banco separadamente.\n"
                  << "4. **Contenção:** throughput deixa de crescer enquanto lock wait aumenta; use "
                     "profiling concorrente e verifique ordem/duração das regiões críticas.\n"
                  << "5. **Dependência lenta:** tempo externo ou consulta domina o trace, ainda que a "
                     "CPU local esteja ociosa.\n\n"
                  << "**Experimento em ordem causal**\n"
                  << "1. Faça uma linha de base com uma conexão e depois aumente a carga em degraus, "
                     "sem mudar mais nenhuma variável.\n"
                  << "2. Em cada degrau, correlacione latência com CPU por núcleo, RAM, paginação, fila "
                     "de disco, rede, tamanho das filas, waits de lock e tempo das dependências.\n"
                  << "3. Capture flame graph ou amostragem somente no primeiro degrau em que o p95 "
                     "diverge; médias globais escondem caudas.\n"
                  << "4. Escolha a hipótese que prevê um sinal exclusivo. Aplique uma intervenção "
                     "reversível — por exemplo, ampliar um pool só se a fila desse pool estiver cheia.\n"
                  << "5. Repita a mesma carga e aceite a causa apenas se o sinal e o p95 mudarem juntos; "
                     "restaure a alteração para confirmar o efeito.\n\n"
                  << "**Resultado esperado**\n"
                  << "O produto da investigação não é apenas 'mais rápido', mas uma curva carga × "
                     "latência, o recurso limitante, a evidência que refutou alternativas e um teste de "
                     "regressão reproduzível.";
        if (!evidence.empty()) {
            diagnosis << "\n\nBase relacionada: " << evidence.front().title << ".";
        }
        return diagnosis.str();
    }

    const std::vector<std::string> statements = split_sentences(prompt);
    const std::vector<std::string> terms = significant_words(prompt);
    std::ostringstream output;
    output << "**Decomposição do problema**\n";
    if (!statements.empty()) {
        const std::size_t count = std::min<std::size_t>(statements.size(), 5U);
        for (std::size_t index = 0U; index < count; ++index) {
            output << "- Afirmação/requisito " << index + 1U << ": " << statements[index] << '\n';
        }
    } else {
        output << "- Objetivo: " << trim_copy(prompt) << '\n';
    }
    output << "\n**Método verificável**\n"
           << "1. Definir o resultado observável e o que contaria como falha.\n"
           << "2. Separar fatos fornecidos, hipóteses e variáveis ainda desconhecidas.\n"
           << "3. Criar pelo menos duas explicações ou estratégias concorrentes.\n"
           << "4. Escolher o teste mais barato capaz de distinguir essas alternativas.\n"
           << "5. Atualizar a conclusão com o resultado, registrando incerteza e limites.\n";
    if (!terms.empty()) {
        output << "\nSinais centrais identificados: ";
        const std::size_t count = std::min<std::size_t>(terms.size(), 8U);
        for (std::size_t index = 0U; index < count; ++index) {
            if (index != 0U) output << ", ";
            output << terms[index];
        }
        output << ".\n";
    }
    if (!evidence.empty()) {
        output << "\n**Base disponível — " << evidence.front().title << "**\n"
               << evidence.front().text << '\n';
    } else {
        output << "\nNão há evidência factual recuperada para decidir o resultado específico; o plano "
                  "acima organiza a investigação sem fabricar uma conclusão.";
    }
    return output.str();
}

[[nodiscard]] std::string grounded_answer(NativeCognitiveIntent intent,
                                          std::string_view prompt,
                                          const std::vector<Evidence>& evidence,
                                          PowerMode power_mode) {
    if (evidence.empty()) return {};
    const std::string topic = topic_from_prompt(prompt);
    std::ostringstream output;
    if (intent == NativeCognitiveIntent::long_form_writing) {
        const std::size_t wanted = power_mode == PowerMode::flash ? 1U
                                   : power_mode == PowerMode::medium ? 3U : 5U;
        const std::size_t count = std::min(wanted, evidence.size());
        output << "# " << topic << "\n\n"
               << "## Introdução\n"
               << "Uma análise útil de " << topic
               << " precisa separar conceitos, consequências práticas e limites da evidência. "
                  "O texto a seguir organiza somente o conhecimento recuperado, sem apresentar "
                  "inferências como fatos confirmados.\n";
        for (std::size_t index = 0U; index < count; ++index) {
            output << "\n## " << evidence[index].title << "\n"
                   << evidence[index].text << "\n";
        }
        output << "\n## Síntese crítica\n"
               << "As perspectivas acima são complementares quando respondem a partes diferentes "
                  "do problema. Antes de escolher uma conclusão, convém identificar o objetivo, "
                  "comparar custos e benefícios e procurar um teste observável que possa contrariar "
                  "a hipótese preferida. Essa disciplina reduz decisões baseadas apenas em confiança "
                  "retórica.\n\n"
               << "## Limites e contrapontos\n"
               << "O material recuperado é uma base inicial, não uma revisão exaustiva. Conceitos "
                  "estáveis podem orientar o raciocínio, mas datas, números atuais e afirmações "
                  "controversas devem ser confirmados em fontes independentes. Também é necessário "
                  "distinguir correlação, mecanismo causal e preferência de valor.\n\n"
               << "## Conclusão\n"
               << "A conclusão mais defensável é condicional: use os princípios apresentados para "
                  "formular uma hipótese específica, registre os critérios de sucesso e revise o "
                  "texto quando novas evidências surgirem. Assim, " << topic
               << " deixa de ser apenas um tema amplo e se torna uma investigação verificável.\n\n"
               << "Base utilizada: ";
        std::set<std::string> sources;
        for (std::size_t index = 0U; index < count; ++index) {
            sources.insert(evidence[index].source);
        }
        std::size_t emitted = 0U;
        for (const std::string& source : sources) {
            if (emitted++ != 0U) output << ", ";
            output << source;
        }
        output << '.';
        return output.str();
    }
    if (intent == NativeCognitiveIntent::comparison) {
        output << "**Comparação: " << topic << "**\n\n";
        const std::size_t count = std::min<std::size_t>(evidence.size(), 3U);
        for (std::size_t index = 0U; index < count; ++index) {
            output << "**" << evidence[index].title << "**\n"
                   << evidence[index].text << "\n\n";
        }
        output << "**Critério de escolha**\nCompare objetivo, custo de tempo e memória, riscos, "
                  "manutenção e possibilidade de testar. A melhor opção depende da operação dominante, "
                  "não apenas de uma vantagem isolada.";
        return output.str();
    }
    if (intent == NativeCognitiveIntent::procedure) {
        output << "**Como abordar " << topic << "**\n\n"
               << evidence.front().text << "\n\n**Passo a passo**\n"
               << "1. Defina o objetivo, as entradas disponíveis e um critério de conclusão.\n"
               << "2. Construa a menor versão capaz de ser testada.\n"
               << "3. Registre o resultado antes de mudar outra variável.\n"
               << "4. Trate erros e limites encontrados no teste.\n"
               << "5. Repita e compare a métrica com a linha de base.\n";
        if (evidence.size() > 1U && power_mode != PowerMode::flash) {
            output << "\n**Conexão útil — " << evidence[1].title << "**\n"
                   << evidence[1].text << '\n';
        }
        return output.str();
    }
    if (intent == NativeCognitiveIntent::decision) {
        output << "**Decisão sobre " << topic << "**\n\n"
               << "Evidência disponível: " << evidence.front().text << "\n\n"
               << "Use uma matriz simples: atribua peso à qualidade, custo, desempenho, risco e "
                  "manutenção; dê nota de 0 a 5 para cada opção e multiplique pelo peso. Faça também "
                  "um teste pequeno, porque uma pontuação teórica não revela compatibilidade real.";
        return output.str();
    }

    output << "**" << evidence.front().title << "**\n\n"
           << evidence.front().text;
    const std::size_t wanted = power_mode == PowerMode::flash ? 1U
                               : power_mode == PowerMode::medium ? 2U : 4U;
    const std::size_t count = std::min(wanted, evidence.size());
    if (count > 1U) output << "\n\n**Conexões importantes**";
    for (std::size_t index = 1U; index < count; ++index) {
        output << "\n\n- **" << evidence[index].title << ":** " << evidence[index].text;
    }
    output << "\n\nBase utilizada: ";
    std::set<std::string> sources;
    for (std::size_t index = 0U; index < count; ++index) sources.insert(evidence[index].source);
    std::size_t emitted = 0U;
    for (const std::string& source : sources) {
        if (emitted++ != 0U) output << ", ";
        output << source;
    }
    output << ". Para informação recente ou específica, é necessário pesquisar uma fonte atual.";
    return output.str();
}

} // namespace

std::optional<NativeCognitionResult> synthesize_native_response(
    std::string_view prompt,
    std::string_view conversation_context,
    std::span<const MemoryHit> memories,
    const NativeCognitionOptions& options,
    std::span<const CuratedKnowledgeAnswer> supplied_evidence) {
    if (prompt.empty() || prompt.size() > 64U * 1024U
        || options.maximum_output_bytes < 256U || options.maximum_output_bytes > 256U * 1024U
        || options.maximum_evidence > 32U) {
        return std::nullopt;
    }

    const NativeCognitiveIntent intent = detect_intent(prompt);
    std::string answer;
    std::size_t evidence_count = 0U;
    bool used_memory = false;
    bool generated_code = false;
    double confidence = 0.0;

    if (intent == NativeCognitiveIntent::conversation) {
        answer = conversation_answer(prompt, conversation_context);
        confidence = answer.empty() ? 0.0 : 0.95;
    } else if (intent == NativeCognitiveIntent::summarization) {
        answer = summarize_extractively(prompt, options.power_mode);
        confidence = answer.empty() ? 0.0 : 0.90;
    } else if (intent == NativeCognitiveIntent::creative_writing) {
        answer = creative_answer(prompt, options.power_mode);
        confidence = 0.82;
    } else if (intent == NativeCognitiveIntent::brainstorming) {
        answer = brainstorm_answer(prompt, options.power_mode);
        confidence = 0.86;
    } else {
        const std::vector<Evidence> evidence = gather_evidence(
            prompt, memories, supplied_evidence, options.maximum_evidence);
        evidence_count = evidence.size();
        used_memory = std::any_of(evidence.begin(), evidence.end(), [](const Evidence& item) {
            return item.memory;
        });
        if (intent == NativeCognitiveIntent::programming) {
            answer = programming_answer(prompt, evidence);
            generated_code = answer.find("```cpp") != std::string::npos;
            confidence = generated_code ? 0.94 : answer.empty() ? 0.0 : 0.70;
        } else if (intent == NativeCognitiveIntent::reasoning) {
            answer = reasoning_answer(prompt, evidence);
            confidence = evidence.empty() ? 0.62 : 0.76;
        } else {
            answer = grounded_answer(intent, prompt, evidence, options.power_mode);
            confidence = evidence.empty() ? 0.0
                : std::clamp(0.55 + evidence.front().score * 0.40, 0.0, 0.93);
        }
    }

    if (answer.empty()) return std::nullopt;
    truncate_utf8(answer, options.maximum_output_bytes);
    return NativeCognitionResult{
        std::move(answer), intent, confidence, evidence_count, used_memory, generated_code};
}

std::string_view native_cognitive_intent_name(NativeCognitiveIntent intent) noexcept {
    switch (intent) {
    case NativeCognitiveIntent::conversation: return "conversa";
    case NativeCognitiveIntent::explanation: return "explicação fundamentada";
    case NativeCognitiveIntent::comparison: return "comparação";
    case NativeCognitiveIntent::procedure: return "procedimento";
    case NativeCognitiveIntent::decision: return "apoio à decisão";
    case NativeCognitiveIntent::reasoning: return "decomposição verificável";
    case NativeCognitiveIntent::brainstorming: return "ideação";
    case NativeCognitiveIntent::creative_writing: return "escrita criativa";
    case NativeCognitiveIntent::long_form_writing: return "redação longa fundamentada";
    case NativeCognitiveIntent::summarization: return "resumo extrativo";
    case NativeCognitiveIntent::programming: return "engenharia de código";
    }
    return "síntese nativa";
}

std::vector<float> native_feature_embedding(std::string_view text,
                                            std::size_t dimension) {
    if (dimension == 0U || dimension > 65'536U) {
        throw Error("dimensão inválida para embedding cognitivo nativo");
    }
    std::vector<float> embedding(dimension, 0.0F);
    const std::string canonical = canonical_text(text);
    const std::vector<std::string_view> words = split_views(canonical);
    const auto add_feature = [&](std::string_view feature, float weight) {
        const std::uint64_t first = fnv1a(feature);
        const std::uint64_t second = fnv1a(feature, 10'995'116'282'11ULL);
        const std::size_t index = static_cast<std::size_t>(first % dimension);
        const float sign = (second & 1U) == 0U ? 1.0F : -1.0F;
        embedding[index] += weight * sign;
    };

    for (std::size_t index = 0U; index < words.size(); ++index) {
        const std::string_view word = words[index];
        if (word.size() < 2U) continue;
        add_feature(word, 1.0F);
        if (word.size() >= 5U) add_feature(word.substr(0U, std::min<std::size_t>(7U, word.size())), 0.55F);
        for (std::size_t offset = 0U; offset + 3U <= word.size(); ++offset) {
            add_feature(word.substr(offset, 3U), 0.22F);
        }
        if (index + 1U < words.size()) {
            std::string bigram(word);
            bigram.push_back('_');
            bigram.append(words[index + 1U]);
            add_feature(bigram, 0.72F);
        }
    }
    normalize_in_place(embedding);
    return embedding;
}

std::vector<std::string> chunk_knowledge_document(
    std::string_view source,
    std::string_view text,
    std::size_t maximum_chunk_bytes) {
    if (source.empty() || source.size() > 2'048U || text.empty()
        || text.size() > 256U * 1024U * 1024U
        || maximum_chunk_bytes < 512U || maximum_chunk_bytes > 64U * 1024U) {
        throw Error("origem, documento ou tamanho de trecho fora dos limites de ingestão");
    }
    const std::size_t prefix_size = 32U + source.size();
    if (prefix_size >= maximum_chunk_bytes) {
        throw Error("origem longa demais para o tamanho de trecho escolhido");
    }
    const std::size_t payload_limit = maximum_chunk_bytes - prefix_size;
    std::vector<std::string> chunks;
    std::size_t begin = 0U;
    while (begin < text.size()) {
        while (begin < text.size()
               && (text[begin] == ' ' || text[begin] == '\t'
                   || text[begin] == '\r' || text[begin] == '\n')) {
            ++begin;
        }
        if (begin >= text.size()) break;
        std::size_t end = std::min(text.size(), begin + payload_limit);
        if (end < text.size()) {
            while (end > begin
                   && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
                --end;
            }
            const std::size_t paragraph = text.rfind("\n\n", end);
            const std::size_t sentence = text.find_last_of(".!?\n", end);
            const std::size_t minimum_split = begin + payload_limit / 2U;
            if (paragraph != std::string_view::npos && paragraph >= minimum_split) {
                end = paragraph + 2U;
            } else if (sentence != std::string_view::npos && sentence >= minimum_split) {
                end = sentence + 1U;
            }
            if (end <= begin) end = std::min(text.size(), begin + payload_limit);
        }
        std::string payload = trim_copy(text.substr(begin, end - begin));
        if (!payload.empty()) {
            chunks.push_back("[DOC]\nFonte: " + std::string(source)
                             + "\nConteúdo: " + std::move(payload));
        }
        begin = end;
    }
    if (chunks.empty()) throw Error("documento não possui conteúdo textual ingerível");
    return chunks;
}

} // namespace cortex
