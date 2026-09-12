#include "cortex/self_editor.hpp"

#include "cortex/core.hpp"
#include "cortex/quality.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace cortex {
namespace {

constexpr std::size_t maximum_source_bytes = 192U * 1024U;
constexpr std::size_t maximum_unrestricted_file_bytes = 8U * 1024U * 1024U;
constexpr std::size_t maximum_instruction_bytes = 4U * 1024U;
constexpr std::size_t maximum_unrestricted_instruction_bytes = 64U * 1024U;
constexpr std::size_t maximum_replacement_bytes = 256U * 1024U;
constexpr std::size_t maximum_unrestricted_replacement_bytes = 8U * 1024U * 1024U;

[[nodiscard]] bool path_begins_with(const std::filesystem::path& path,
                                    const std::filesystem::path& root) {
    auto path_part = path.begin();
    auto root_part = root.begin();
    for (; root_part != root.end(); ++root_part, ++path_part) {
        if (path_part == path.end() || *path_part != *root_part) return false;
    }
    return true;
}

[[nodiscard]] bool allowed_extension(const std::filesystem::path& path) {
    constexpr std::array<std::string_view, 5U> extensions{
        ".cpp", ".hpp", ".md", ".txt", ".css"
    };
    const std::string extension = path.extension().string();
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

[[nodiscard]] bool allowed_top_directory(const std::filesystem::path& relative) {
    if (relative.empty()) return false;
    const std::string first = relative.begin()->string();
    return first == "src" || first == "include" || first == "tests" || first == "data";
}

[[nodiscard]] std::string trim_copy(std::string_view value) {
    const auto whitespace = [](char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    };
    while (!value.empty() && whitespace(value.front())) value.remove_prefix(1U);
    while (!value.empty() && whitespace(value.back())) value.remove_suffix(1U);
    return std::string(value);
}

[[nodiscard]] std::size_t occurrences(std::string_view text,
                                      std::string_view needle) noexcept {
    if (needle.empty()) return 0U;
    std::size_t count = 0U;
    std::size_t offset = 0U;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

[[nodiscard]] std::string join_findings(const std::vector<std::string>& findings) {
    if (findings.empty()) return "nenhum alerta estático relevante";
    std::ostringstream output;
    for (std::size_t index = 0U; index < findings.size(); ++index) {
        if (index != 0U) output << "; ";
        output << findings[index];
    }
    return output.str();
}

[[nodiscard]] bool source_extension(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".cpp" || extension == ".hpp";
}

// Validação léxica conservadora: não substitui um compilador, mas bloqueia propostas
// truncadas, delimitadores incompatíveis e strings/comentários sem fechamento.
void validate_balanced_cpp(std::string_view source) {
    enum class State : std::uint8_t { normal, line_comment, block_comment, string, character };
    State state = State::normal;
    bool escaped = false;
    std::vector<char> delimiters;
    for (std::size_t index = 0U; index < source.size(); ++index) {
        const char current = source[index];
        const char next = index + 1U < source.size() ? source[index + 1U] : '\0';
        if (state == State::line_comment) {
            if (current == '\n') state = State::normal;
            continue;
        }
        if (state == State::block_comment) {
            if (current == '*' && next == '/') {
                state = State::normal;
                ++index;
            }
            continue;
        }
        if (state == State::string || state == State::character) {
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if ((state == State::string && current == '"')
                       || (state == State::character && current == '\'')) {
                state = State::normal;
            }
            continue;
        }
        if (current == '/' && next == '/') {
            state = State::line_comment;
            ++index;
            continue;
        }
        if (current == '/' && next == '*') {
            state = State::block_comment;
            ++index;
            continue;
        }
        if (current == 'R' && next == '"') {
            const std::size_t opening = source.find('(', index + 2U);
            if (opening == std::string_view::npos || opening - (index + 2U) > 16U) {
                throw Error("proposta contém raw string C++ inválida");
            }
            const std::string delimiter(source.substr(index + 2U, opening - index - 2U));
            const std::string closing = ")" + delimiter + "\"";
            const std::size_t end = source.find(closing, opening + 1U);
            if (end == std::string_view::npos) {
                throw Error("proposta contém raw string C++ sem fechamento");
            }
            index = end + closing.size() - 1U;
            continue;
        }
        if (current == '"') {
            state = State::string;
            continue;
        }
        if (current == '\'') {
            state = State::character;
            continue;
        }
        if (current == '{' || current == '(' || current == '[') {
            delimiters.push_back(current);
            continue;
        }
        if (current == '}' || current == ')' || current == ']') {
            const char expected = current == '}' ? '{' : current == ')' ? '(' : '[';
            if (delimiters.empty() || delimiters.back() != expected) {
                throw Error("proposta contém delimitadores C++ incompatíveis");
            }
            delimiters.pop_back();
        }
    }
    if (state == State::block_comment || state == State::string
        || state == State::character || !delimiters.empty()) {
        throw Error("proposta C++ parece truncada ou possui delimitador sem fechamento");
    }
}

constexpr std::string_view rollback_magic = "CORTEX_ROLLBACK_V2";

struct RollbackMetadata final {
    std::string target_path;
    std::string backup_name;
    std::uint64_t applied_hash = 0U;
    bool created_file = false;
};

[[nodiscard]] std::filesystem::path rollback_manifest(
    const std::filesystem::path& backup_directory, std::uint64_t proposal_id) {
    return backup_directory / (std::to_string(proposal_id) + ".rollback");
}

[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) return false;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

void write_rollback_metadata(const std::filesystem::path& manifest,
                             const RollbackMetadata& metadata) {
    if (std::filesystem::exists(manifest)) {
        throw Error("registro de rollback já existe; gere uma nova proposta");
    }
    const std::filesystem::path temporary = temporary_path_for(manifest);
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << rollback_magic << '\n' << metadata.target_path << '\n'
               << metadata.backup_name << '\n' << metadata.applied_hash << '\n'
               << (metadata.created_file ? "1" : "0") << '\n';
        output.flush();
        if (!output) throw Error("não foi possível registrar rollback persistente");
        output.close();
        replace_file(temporary, manifest);
    } catch (...) {
        std::filesystem::remove(temporary);
        throw;
    }
}

[[nodiscard]] RollbackMetadata read_rollback_metadata(
    const std::filesystem::path& manifest) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(manifest, error);
    if (error || size == 0U || size > 16U * 1024U
        || std::filesystem::is_symlink(manifest, error)) {
        throw Error("registro de rollback ausente ou inválido");
    }
    std::ifstream input(manifest, std::ios::binary);
    std::string magic;
    RollbackMetadata metadata;
    std::string hash;
    std::string created;
    if (!std::getline(input, magic) || !std::getline(input, metadata.target_path)
        || !std::getline(input, metadata.backup_name) || !std::getline(input, hash)
        || !std::getline(input, created) || magic != rollback_magic
        || !parse_u64(hash, metadata.applied_hash)
        || (created != "0" && created != "1")) {
        throw Error("registro de rollback está corrompido");
    }
    metadata.created_file = created == "1";
    std::string unexpected;
    if (std::getline(input, unexpected) && !unexpected.empty()) {
        throw Error("registro de rollback contém dados inesperados");
    }
    const std::filesystem::path backup_name(metadata.backup_name);
    if (backup_name.empty() || backup_name.is_absolute() || backup_name.has_parent_path()) {
        throw Error("registro de rollback contém caminho de backup inválido");
    }
    return metadata;
}

[[nodiscard]] std::optional<std::string> fenced_replacement(std::string_view instruction) {
    const std::size_t opening = instruction.find("```");
    if (opening == std::string_view::npos) return std::nullopt;
    const std::size_t line = instruction.find('\n', opening + 3U);
    if (line == std::string_view::npos) return std::nullopt;
    const std::size_t closing = instruction.find("```", line + 1U);
    if (closing == std::string_view::npos) return std::nullopt;
    std::string content = trim_copy(instruction.substr(line + 1U, closing - line - 1U));
    if (content.empty()) return std::nullopt;
    content.push_back('\n');
    return content;
}

[[nodiscard]] std::string lowercase_ascii(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
        return character >= static_cast<unsigned char>('A')
                   && character <= static_cast<unsigned char>('Z')
            ? static_cast<char>(character + 32U) : static_cast<char>(character);
    });
    return lowered;
}

[[nodiscard]] std::string clean_source_layout(std::string_view original) {
    std::ostringstream output;
    std::size_t begin = 0U;
    std::size_t blank_run = 0U;
    while (begin <= original.size()) {
        const std::size_t newline = original.find('\n', begin);
        std::string_view line = original.substr(
            begin, (newline == std::string_view::npos ? original.size() : newline) - begin);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'
                                  || line.back() == '\r')) {
            line.remove_suffix(1U);
        }
        const bool blank = line.empty();
        if (!blank || blank_run < 2U) output << line << '\n';
        blank_run = blank ? blank_run + 1U : 0U;
        if (newline == std::string_view::npos) break;
        begin = newline + 1U;
    }
    std::string cleaned = output.str();
    while (cleaned.size() > 1U && cleaned.ends_with("\n\n\n")) cleaned.pop_back();
    return cleaned;
}

[[nodiscard]] std::optional<std::pair<std::string, std::string>> inline_substitution(
    std::string_view instruction) {
    const std::string lowered = lowercase_ascii(instruction);
    if (lowered.find("substitua") == std::string::npos
        && lowered.find("replace") == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t first = instruction.find('`');
    const std::size_t second = first == std::string_view::npos
                               ? first : instruction.find('`', first + 1U);
    const std::size_t third = second == std::string_view::npos
                              ? second : instruction.find('`', second + 1U);
    const std::size_t fourth = third == std::string_view::npos
                               ? third : instruction.find('`', third + 1U);
    if (first == std::string_view::npos || second == std::string_view::npos
        || third == std::string_view::npos || fourth == std::string_view::npos
        || second == first + 1U) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string>{
        std::string(instruction.substr(first + 1U, second - first - 1U)),
        std::string(instruction.substr(third + 1U, fourth - third - 1U))};
}

[[nodiscard]] std::string native_edit_replacement(const std::filesystem::path& path,
                                                   std::string_view original,
                                                   std::string_view instruction,
                                                   bool creates_file) {
    const std::string lowered = lowercase_ascii(instruction);
    const bool declares_complete_content = creates_file
        || lowered.find("conteúdo completo") != std::string::npos
        || lowered.find("conteudo completo") != std::string::npos
        || lowered.find("arquivo completo") != std::string::npos
        || lowered.find("full file") != std::string::npos;
    if (declares_complete_content) {
        if (const auto fenced = fenced_replacement(instruction)) return *fenced;
    }

    if (const auto substitution = inline_substitution(instruction)) {
        if (substitution->first.empty()) throw Error("texto antigo da substituição está vazio");
        const std::size_t found = original.find(substitution->first);
        if (found == std::string_view::npos) {
            throw Error("motor nativo não encontrou exatamente o trecho entre crases");
        }
        std::string replacement(original);
        replacement.replace(found, substitution->first.size(), substitution->second);
        return replacement;
    }

    const bool cleanup = lowered.find("espaço") != std::string::npos
                         || lowered.find("espaco") != std::string::npos
                         || lowered.find("linha em branco") != std::string::npos
                         || lowered.find("formate") != std::string::npos
                         || lowered.find("formatar") != std::string::npos
                         || lowered.find("limpe") != std::string::npos;
    if (!creates_file && cleanup) {
        const std::string replacement = clean_source_layout(original);
        if (replacement != original) return replacement;
    }

    const std::size_t include = instruction.find("#include ");
    if (!creates_file && include != std::string_view::npos) {
        const std::size_t include_end = instruction.find('\n', include);
        const std::string directive = trim_copy(instruction.substr(
            include, (include_end == std::string_view::npos ? instruction.size() : include_end)
                         - include));
        if (!directive.empty() && original.find(directive) == std::string_view::npos) {
            std::string replacement(original);
            std::size_t insertion = 0U;
            std::size_t cursor = 0U;
            while (cursor < replacement.size()) {
                const std::size_t line_end = replacement.find('\n', cursor);
                const std::string_view line = replacement.substr(
                    cursor, (line_end == std::string::npos ? replacement.size() : line_end) - cursor);
                if (line.starts_with("#include ")) insertion = line_end == std::string::npos
                    ? replacement.size() : line_end + 1U;
                else if (insertion != 0U && !line.empty()) break;
                if (line_end == std::string::npos) break;
                cursor = line_end + 1U;
            }
            replacement.insert(insertion, directive + "\n");
            return replacement;
        }
    }

    if (creates_file && (path.extension() == ".md" || path.extension() == ".txt")) {
        constexpr std::array<std::string_view, 4U> markers{
            "CONTEÚDO:", "Conteúdo:", "CONTEUDO:", "Conteudo:"
        };
        for (const std::string_view marker : markers) {
            const std::size_t found = instruction.find(marker);
            if (found != std::string_view::npos) {
                std::string replacement = trim_copy(instruction.substr(found + marker.size()));
                if (!replacement.empty()) {
                    replacement.push_back('\n');
                    return replacement;
                }
            }
        }
    }

    throw Error(
        "o motor de autoedição nativo precisa de conteúdo completo em bloco ```...```, "
        "de `substitua `trecho antigo` por `trecho novo``, de uma diretiva #include "
        "ou de uma limpeza auditável; para mudanças generativas use o perfil Granite local");
}

[[nodiscard]] std::string normalized_newlines(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] == '\r' && index + 1U < text.size() && text[index + 1U] == '\n') {
            continue;
        }
        result.push_back(text[index]);
    }
    return result;
}

[[nodiscard]] std::string crlf_newlines(std::string_view text) {
    std::string result;
    result.reserve(text.size() + text.size() / 16U);
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] == '\n' && (index == 0U || text[index - 1U] != '\r')) {
            result.push_back('\r');
        }
        result.push_back(text[index]);
    }
    return result;
}

[[nodiscard]] std::optional<std::string> apply_search_replace_patch(
    std::string_view completion,
    std::string_view original,
    std::size_t maximum_bytes) {
    constexpr std::string_view opening = "<<<<<<< SEARCH\n";
    constexpr std::string_view separator = "\n=======\n";
    constexpr std::string_view closing = "\n>>>>>>> REPLACE";
    std::string patch = normalized_newlines(completion);
    if (patch.find("<<<<<<< SEARCH") == std::string::npos) return std::nullopt;

    std::string replacement(original);
    const bool original_uses_crlf = original.find("\r\n") != std::string_view::npos;
    std::size_t cursor = 0U;
    std::size_t blocks = 0U;
    while (true) {
        const std::size_t begin = patch.find(opening, cursor);
        if (begin == std::string::npos) break;
        const std::size_t search_begin = begin + opening.size();
        const std::size_t middle = patch.find(separator, search_begin);
        if (middle == std::string::npos) {
            throw Error("patch do modelo possui bloco SEARCH sem separador");
        }
        const std::size_t replacement_begin = middle + separator.size();
        const std::size_t end = patch.find(closing, replacement_begin);
        if (end == std::string::npos) {
            throw Error("patch do modelo possui bloco REPLACE sem fechamento");
        }
        std::string search = patch.substr(search_begin, middle - search_begin);
        std::string inserted = patch.substr(replacement_begin, end - replacement_begin);
        if (search.empty()) throw Error("patch do modelo possui busca vazia");
        if (original_uses_crlf) {
            search = crlf_newlines(search);
            inserted = crlf_newlines(inserted);
        }
        const std::size_t found = replacement.find(search);
        if (found == std::string::npos) {
            throw Error("patch do modelo não corresponde exatamente ao arquivo atual");
        }
        if (replacement.find(search, found + search.size()) != std::string::npos) {
            throw Error("patch do modelo usa trecho SEARCH ambíguo; inclua mais contexto");
        }
        replacement.replace(found, search.size(), inserted);
        if (replacement.size() > maximum_bytes) {
            throw Error("patch do modelo excede o limite do arquivo");
        }
        cursor = end + closing.size();
        ++blocks;
        if (blocks > 32U) throw Error("patch do modelo contém blocos demais");
    }
    if (blocks == 0U) throw Error("patch SEARCH/REPLACE malformado");
    if (occurrences(patch, "<<<<<<< SEARCH") != blocks) {
        throw Error("patch do modelo contém marcador SEARCH incompleto ou inesperado");
    }
    return replacement;
}

[[nodiscard]] std::string source_excerpt_for_edit(std::string_view source,
                                                  std::string_view instruction) {
    constexpr std::size_t complete_limit = 5U * 1024U;
    if (source.size() <= complete_limit) return std::string(source);

    struct Range final { std::size_t begin; std::size_t end; };
    std::vector<Range> ranges;
    ranges.push_back(Range{0U, std::min<std::size_t>(source.size(), 1'000U)});
    const std::string lowered_source = lowercase_ascii(source);
    std::vector<std::string> terms = significant_words(instruction);
    std::sort(terms.begin(), terms.end(), [](const std::string& left,
                                             const std::string& right) {
        return left.size() != right.size() ? left.size() > right.size() : left < right;
    });
    std::size_t selected = 0U;
    for (const std::string& term : terms) {
        if (term.size() < 3U) continue;
        const std::size_t found = lowered_source.find(lowercase_ascii(term));
        if (found == std::string::npos) continue;
        const std::size_t begin = found > 900U ? found - 900U : 0U;
        const std::size_t end = std::min<std::size_t>(source.size(), found + term.size() + 900U);
        ranges.push_back(Range{begin, end});
        if (++selected == 2U) break;
    }
    if (selected == 0U) {
        const std::size_t middle = source.size() / 2U;
        ranges.push_back(Range{middle > 900U ? middle - 900U : 0U,
                               std::min<std::size_t>(source.size(), middle + 900U)});
    }
    ranges.push_back(Range{source.size() > 600U ? source.size() - 600U : 0U, source.size()});

    for (Range& range : ranges) {
        if (range.begin != 0U) {
            const std::size_t newline = source.rfind('\n', range.begin);
            if (newline != std::string_view::npos) range.begin = newline + 1U;
        }
        if (range.end < source.size()) {
            const std::size_t newline = source.find('\n', range.end);
            if (newline != std::string_view::npos) range.end = newline + 1U;
        }
    }
    std::sort(ranges.begin(), ranges.end(), [](const Range& left, const Range& right) {
        return left.begin < right.begin;
    });
    std::vector<Range> merged;
    for (const Range& range : ranges) {
        if (!merged.empty() && range.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, range.end);
        } else {
            merged.push_back(range);
        }
    }

    std::ostringstream excerpt;
    for (std::size_t index = 0U; index < merged.size(); ++index) {
        excerpt << "[TRECHO " << (index + 1U) << " | bytes " << merged[index].begin
                << ".." << merged[index].end << "]\n"
                << source.substr(merged[index].begin, merged[index].end - merged[index].begin)
                << "\n[FIM DO TRECHO]\n";
    }
    return excerpt.str();
}

} // namespace

SafeSelfEditor::SafeSelfEditor(std::filesystem::path source_root,
                               const LanguageApiClient& language_api,
                               bool enabled,
                               bool unrestricted_scope)
    : language_api_(language_api),
      enabled_(enabled),
      unrestricted_scope_(enabled && unrestricted_scope) {
    if (!source_root.empty()) {
        root_ = std::filesystem::weakly_canonical(std::move(source_root));
    }
    if (enabled_ && (root_.empty() || !std::filesystem::is_directory(root_))) {
        throw Error("--source-root não aponta para o projeto Cortex");
    }
    // IDs sobrevivem a reinícios: registros antigos nunca podem ser sobrescritos por
    // uma nova proposta que recomeçaria em 1.
    if (enabled_) {
        const std::filesystem::path backup_directory = root_ / ".cortex-backups";
        std::error_code error;
        for (std::filesystem::directory_iterator iterator(backup_directory, error), end;
             !error && iterator != end; iterator.increment(error)) {
            const std::filesystem::path path = iterator->path();
            if (path.extension() != ".rollback") continue;
            std::uint64_t id = 0U;
            if (parse_u64(path.stem().string(), id) && id >= next_id_
                && id != std::numeric_limits<std::uint64_t>::max()) {
                next_id_ = id + 1U;
            }
        }
    }
}

std::filesystem::path SafeSelfEditor::resolve_target(std::string_view path,
                                                       bool require_existing) const {
    if (!enabled_) throw Error("autoedição está desativada; use --enable-self-edit");
    const std::size_t path_limit = unrestricted_scope_ ? 4'096U : 512U;
    if (path.empty() || path.size() > path_limit) {
        throw Error("caminho de autoedição vazio ou longo demais");
    }
    for (const unsigned char character : path) {
        if (character < 0x20U || character == 0x7FU) {
            throw Error("caminho de autoedição contém caractere de controle");
        }
    }

    const std::filesystem::path requested{std::string(path)};
    if (!unrestricted_scope_) {
        if (!require_existing || requested.is_absolute() || !allowed_top_directory(requested)
            || !allowed_extension(requested)) {
            throw Error("modo controlado aceita apenas fontes existentes em src/include/tests/data");
        }
        for (const auto& component : requested) {
            if (component == ".." || component == ".") {
                throw Error("componentes relativos não são aceitos na autoedição controlada");
            }
        }
        const std::filesystem::path resolved = std::filesystem::weakly_canonical(root_ / requested);
        if (!path_begins_with(resolved, root_) || !std::filesystem::is_regular_file(resolved)
            || std::filesystem::is_symlink(root_ / requested)) {
            throw Error("arquivo de autoedição ausente, simbólico ou fora do projeto");
        }
        return resolved;
    }

    // No modo irrestrito supervisionado, o caminho pode ser absoluto e sair do
    // projeto. A autoridade real continua sendo a conta/UAC do sistema operacional.
    const std::filesystem::path candidate = requested.is_absolute()
        ? requested : root_ / requested;
    if (candidate.filename().empty()) {
        throw Error("o destino irrestrito deve identificar um arquivo, não um diretório");
    }
    if (std::filesystem::exists(candidate)) {
        if (!std::filesystem::is_regular_file(candidate)
            || std::filesystem::is_symlink(candidate)) {
            throw Error("destino irrestrito existente não é um arquivo regular");
        }
        return std::filesystem::weakly_canonical(candidate);
    }
    if (require_existing) throw Error("arquivo solicitado não existe");
    const std::filesystem::path parent = std::filesystem::weakly_canonical(candidate.parent_path());
    return (parent / candidate.filename()).lexically_normal();
}

std::string SafeSelfEditor::read_source(const std::filesystem::path& path,
                                         std::size_t maximum_bytes) {
    const std::uintmax_t size = std::filesystem::file_size(path);
    if (size > maximum_bytes) {
        throw Error("arquivo grande demais para autoedição");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw Error("não foi possível ler fonte para autoedição");
    std::string content(static_cast<std::size_t>(size), '\0');
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) throw Error("leitura incompleta do arquivo-fonte");
    return content;
}

std::uint64_t SafeSelfEditor::content_hash(std::string_view content) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const unsigned char byte : content) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

SourceAudit SafeSelfEditor::audit(std::string_view relative_path,
                                  std::string_view observed_behavior) const {
    const std::filesystem::path resolved = resolve_target(relative_path, true);
    const std::string source = read_source(
        resolved, unrestricted_scope_ ? maximum_unrestricted_file_bytes : maximum_source_bytes);
    SourceAudit report;
    report.relative_path = std::string(relative_path);
    report.bytes = source.size();
    report.lines = 1U;
    std::size_t line_length = 0U;
    for (const char character : source) {
        if (character == '\n') {
            if (line_length > 120U) ++report.long_lines;
            ++report.lines;
            line_length = 0U;
        } else if (character != '\r') {
            ++line_length;
        }
    }
    if (line_length > 120U) ++report.long_lines;
    report.todo_markers = occurrences(source, "TODO") + occurrences(source, "FIXME");
    constexpr std::array<std::string_view, 6U> risky_patterns{
        "std::system(", "popen(", "_popen(", "ShellExecute", "WinExec(", "eval("
    };
    for (const std::string_view pattern : risky_patterns) {
        report.risky_calls += occurrences(source, pattern);
    }
    if (report.long_lines != 0U) {
        report.findings.push_back(std::to_string(report.long_lines)
                                  + " linha(s) acima de 120 colunas");
    }
    if (report.todo_markers != 0U) {
        report.findings.push_back(std::to_string(report.todo_markers)
                                  + " marcador(es) TODO/FIXME");
    }
    if (report.risky_calls != 0U) {
        report.findings.push_back(std::to_string(report.risky_calls)
                                  + " chamada(s) de processo/avaliação que exigem revisão");
    }
    if (report.lines > 1'200U) {
        report.findings.emplace_back("arquivo grande; considere separar responsabilidades");
    }
    if (!observed_behavior.empty()) {
        const TextMetrics behavior = analyze_text(observed_behavior);
        report.findings.push_back("comportamento observado: " + std::to_string(behavior.words)
                                  + " palavras em " + std::to_string(behavior.sentences)
                                  + " frases");
    }

    const double long_penalty = std::min(25.0, static_cast<double>(report.long_lines) * 1.5);
    const double todo_penalty = std::min(20.0, static_cast<double>(report.todo_markers) * 3.0);
    const double risk_penalty = std::min(35.0, static_cast<double>(report.risky_calls) * 8.0);
    const double size_penalty = report.lines > 1'200U ? 15.0 : report.lines > 700U ? 7.0 : 0.0;
    report.maintainability_score = std::clamp(
        100.0 - long_penalty - todo_penalty - risk_penalty - size_penalty, 0.0, 100.0);

    std::ostringstream instruction;
    instruction << "Preserve as APIs e o comportamento correto de " << relative_path
                << ". Corrija de forma mínima os achados da auditoria: "
                << join_findings(report.findings)
                << ". Não adicione execução por shell, Python, Node.js ou dependências novas; "
                   "mantenha C++20 e atualize testes relevantes.";
    report.suggested_instruction = instruction.str();
    return report;
}

std::string SafeSelfEditor::extract_replacement(std::string_view completion,
                                                 std::size_t maximum_bytes) {
    const std::size_t opening = completion.find("```");
    if (opening != std::string_view::npos) {
        const std::size_t line = completion.find('\n', opening + 3U);
        const std::size_t closing = line == std::string_view::npos
                                    ? std::string_view::npos
                                    : completion.find("```", line + 1U);
        if (line != std::string_view::npos && closing != std::string_view::npos) {
            completion = completion.substr(line + 1U, closing - line - 1U);
        }
    }
    std::string replacement = trim_copy(completion);
    if (replacement.empty() || replacement.size() > maximum_bytes
        || replacement.find('\0') != std::string::npos) {
        throw Error("API devolveu substituição vazia, binária ou grande demais");
    }
    replacement.push_back('\n');
    return replacement;
}

void SafeSelfEditor::validate_replacement(const std::filesystem::path& path,
                                          std::string_view original,
                                          std::string_view replacement) {
    if (replacement == original) throw Error("a proposta não altera o arquivo");
    if (replacement.empty() || replacement.size() > maximum_replacement_bytes) {
        throw Error("substituição fora dos limites de tamanho");
    }
    if (original.size() > 512U && replacement.size() * 5U < original.size()) {
        throw Error("proposta remove mais de 80% do arquivo; possível truncamento");
    }
    constexpr std::array<std::string_view, 6U> forbidden_new_calls{
        "std::system(", "popen(", "_popen(", "ShellExecute", "WinExec(", "eval("
    };
    for (const std::string_view pattern : forbidden_new_calls) {
        if (occurrences(replacement, pattern) > occurrences(original, pattern)) {
            throw Error("proposta introduz execução dinâmica proibida: " + std::string(pattern));
        }
    }
    if (source_extension(path)) validate_balanced_cpp(replacement);
}

CodeProposal SafeSelfEditor::propose(std::string_view relative_path,
                                     std::string_view instruction) {
    return propose_file(relative_path, instruction, false);
}

CodeProposal SafeSelfEditor::propose_file(std::string_view path,
                                          std::string_view instruction,
                                          bool create_if_missing) {
    const std::size_t instruction_limit = unrestricted_scope_
        ? maximum_unrestricted_instruction_bytes : maximum_instruction_bytes;
    if (instruction.empty() || instruction.size() > instruction_limit) {
        throw Error("instrução de autoedição vazia ou grande demais");
    }
    if (create_if_missing && !unrestricted_scope_) {
        throw Error("criação de arquivo requer --unrestricted-self-edit");
    }

    const std::filesystem::path resolved = resolve_target(path, !create_if_missing);
    const bool creates_file = !std::filesystem::exists(resolved);
    const std::size_t file_limit = unrestricted_scope_
        ? maximum_unrestricted_file_bytes : maximum_source_bytes;
    const std::string original = creates_file ? std::string{} : read_source(resolved, file_limit);
    SourceAudit before;
    if (creates_file) {
        before.relative_path = std::string(path);
        before.maintainability_score = 100.0;
        before.findings.emplace_back("arquivo novo solicitado pelo usuário");
    } else {
        before = audit(path);
    }

    bool generated_by_language_model = false;
    std::string replacement;
    try {
        // Transformações explícitas não gastam inferência e continuam funcionando offline.
        replacement = native_edit_replacement(resolved, original, instruction, creates_file);
    } catch (const Error&) {
        if (!language_api_.configured()) throw;
        generated_by_language_model = true;
        const std::size_t replacement_limit = unrestricted_scope_
            ? maximum_unrestricted_replacement_bytes : maximum_replacement_bytes;
        const std::string system = creates_file
            ? "Você é o motor de evolução supervisionada da Cortex. Crie somente o arquivo solicitado, "
              "sem Python, Node.js, npm ou comandos executados. Devolva o conteúdo completo em um único "
              "bloco de código, sem explicações. O usuário revisará a proposta antes de qualquer escrita."
            : "Você é um engenheiro C++20 trabalhando sob supervisão humana. Faça uma mudança mínima e "
              "causal no arquivo, preserve APIs não relacionadas e não execute comandos. Responda somente "
              "com um ou mais blocos no formato exato:\n<<<<<<< SEARCH\ntrecho antigo exato e único\n"
              "=======\ntrecho novo\n>>>>>>> REPLACE\nO SEARCH deve ser copiado literalmente dos trechos "
              "fornecidos. Não use reticências, números de linha, markdown ou conteúdo fora dos blocos.";
        std::ostringstream user;
        user << "ARQUIVO: " << resolved.string() << "\nOPERAÇÃO: "
             << (creates_file ? "CRIAR" : "EDITAR POR PATCH")
             << "\nINSTRUÇÃO: " << instruction << "\n\n";
        if (creates_file) {
            user << "O ARQUIVO AINDA NÃO EXISTE.";
        } else {
            user << "TRECHOS DO CONTEÚDO ATUAL (sem números de linha; copie SEARCH exatamente):\n"
                 << source_excerpt_for_edit(original, instruction);
        }
        const std::size_t output_tokens = creates_file ? 3'072U : 1'536U;
        const LanguageCompletion completion = language_api_.complete(
            system, user.str(), 0.2F, output_tokens, 20U,
            language_api_.config().supports_reasoning_control
                ? LanguageReasoningMode::low_effort : LanguageReasoningMode::disabled);
        if (!creates_file) {
            const std::optional<std::string> patched = apply_search_replace_patch(
                completion.text, original, replacement_limit);
            if (patched) replacement = *patched;
        }
        // Compatibilidade limitada com modelos antigos: arquivo completo só é aceito quando
        // pequeno e explicitamente cercado por markdown. Fontes grandes nunca podem ser
        // silenciosamente substituídas por uma continuação truncada.
        if (replacement.empty()) {
            if (!creates_file
                && (original.size() > 5U * 1024U
                    || completion.text.find("```") == std::string::npos)) {
                throw Error("o modelo não devolveu patch SEARCH/REPLACE para o arquivo existente");
            }
            replacement = extract_replacement(completion.text, replacement_limit);
        }
    }
    if (unrestricted_scope_) {
        if (replacement.empty() || replacement.size() > maximum_unrestricted_replacement_bytes
            || replacement == original || replacement.find('\0') != std::string::npos) {
            throw Error("proposta irrestrita vazia, binária, idêntica ou acima do limite operacional");
        }
        if (source_extension(resolved)) validate_balanced_cpp(replacement);
    } else {
        validate_replacement(resolved, original, replacement);
    }

    const double size_ratio = static_cast<double>(replacement.size())
                              / static_cast<double>(std::max<std::size_t>(original.size(), 1U));
    CodeProposal proposal;
    {
        std::lock_guard lock(mutex_);
        if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
            throw Error("espaço de IDs de autoedição esgotado");
        }
        proposal.id = next_id_++;
        proposal.relative_path = unrestricted_scope_ ? resolved.string() : std::string(path);
        proposal.instruction = std::string(instruction);
        proposal.replacement = std::move(replacement);
        proposal.original_hash = creates_file ? 0U : content_hash(original);
        std::ostringstream audit_summary;
        audit_summary << (unrestricted_scope_ ? "escopo irrestrito supervisionado; " : "")
                      << (generated_by_language_model
                              ? "proposta por modelo local; "
                              : "transformação determinística nativa; ")
                      << "manutenibilidade " << std::fixed << std::setprecision(1)
                      << before.maintainability_score << "/100; "
                      << join_findings(before.findings);
        proposal.audit_summary = audit_summary.str();
        proposal.risk_level = unrestricted_scope_ ? "unrestricted"
                              : size_ratio < 0.70 || size_ratio > 1.45 ? "high"
                              : size_ratio < 0.90 || size_ratio > 1.15 ? "medium" : "low";
        proposal.creates_file = creates_file;
        proposal.unrestricted_scope = unrestricted_scope_;
        proposals_[proposal.id] = proposal;
    }
    return proposal;
}

CodeProposal SafeSelfEditor::propose_refinement(std::string_view relative_path,
                                                std::string_view objective,
                                                std::string_view observed_behavior) {
    if (objective.empty() || objective.size() > 1'024U) {
        throw Error("objetivo de autorrefinamento vazio ou grande demais");
    }
    if (observed_behavior.size() > 1'024U) {
        throw Error("comportamento observado excede 1 KiB");
    }
    const SourceAudit report = audit(relative_path, observed_behavior);
    std::ostringstream instruction;
    instruction << "OBJETIVO DE AUTORREFINAMENTO: " << objective
                << "\nAUDITORIA NATIVA: " << join_findings(report.findings)
                << "\nINSTRUÇÃO SUGERIDA: " << report.suggested_instruction;
    if (!observed_behavior.empty()) {
        instruction << "\nCOMPORTAMENTO A CORRIGIR:\n" << observed_behavior;
    }
    instruction << "\nFaça uma mudança mínima, causal e testável. Não esconda falhas com respostas fixas.";
    return propose(relative_path, instruction.str());
}

AppliedCodeChange SafeSelfEditor::apply(std::uint64_t proposal_id,
                                        std::string_view confirmation) {
    // Serializar todo o ciclo impede duas confirmações concorrentes do mesmo ID de
    // criarem backups divergentes ou sobrescreverem uma aplicação já concluída.
    std::lock_guard lock(mutex_);
    const auto found = proposals_.find(proposal_id);
    if (found == proposals_.end()) throw Error("proposta de código não encontrada ou expirada");
    const CodeProposal proposal = found->second;
    const std::string expected = "APLICAR " + std::to_string(proposal_id);
    if (confirmation != expected) {
        throw Error("confirmação inválida; digite exatamente: " + expected);
    }

    if (proposal.unrestricted_scope != unrestricted_scope_) {
        throw Error("o modo de acesso mudou após a proposta; gere outra autorização");
    }
    const std::filesystem::path target = resolve_target(
        proposal.relative_path, !proposal.creates_file);
    std::string current;
    if (proposal.creates_file) {
        if (std::filesystem::exists(target)) {
            throw Error("o arquivo passou a existir após a proposta; criação cancelada");
        }
        std::filesystem::create_directories(target.parent_path());
    } else {
        const std::size_t file_limit = unrestricted_scope_
            ? maximum_unrestricted_file_bytes : maximum_source_bytes;
        current = read_source(target, file_limit);
        if (content_hash(current) != proposal.original_hash) {
            throw Error("o arquivo mudou após a proposta; gere outra para evitar conflito");
        }
    }
    if (!unrestricted_scope_) validate_replacement(target, current, proposal.replacement);

    const std::filesystem::path backup_directory = root_ / ".cortex-backups";
    std::filesystem::create_directories(backup_directory);
    std::filesystem::path backup;
    if (!proposal.creates_file) {
        backup = backup_directory
            / (std::to_string(proposal.id) + "-" + target.filename().string());
        for (std::size_t suffix = 1U; std::filesystem::exists(backup); ++suffix) {
            backup = backup_directory / (std::to_string(proposal.id) + "-"
                     + std::to_string(suffix) + "-" + target.filename().string());
        }
        std::ofstream stream(backup, std::ios::binary | std::ios::trunc);
        stream.write(current.data(), static_cast<std::streamsize>(current.size()));
        if (!stream) throw Error("não foi possível criar backup da autoedição");
    }

    const std::filesystem::path manifest = rollback_manifest(backup_directory, proposal_id);
    const RollbackMetadata metadata{
        proposal.relative_path,
        proposal.creates_file ? std::string("-") : backup.filename().string(),
        content_hash(proposal.replacement),
        proposal.creates_file};
    write_rollback_metadata(manifest, metadata);

    const std::filesystem::path temporary = temporary_path_for(target);
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(proposal.replacement.data(),
                     static_cast<std::streamsize>(proposal.replacement.size()));
        stream.flush();
        if (!stream) throw Error("falha ao escrever proposta de autoedição");
        stream.close();
        replace_file(temporary, target);
    } catch (...) {
        std::filesystem::remove(temporary);
        std::filesystem::remove(manifest);
        throw;
    }

    const AppliedCodeChange applied{
        proposal_id, target, backup, metadata.applied_hash, proposal.creates_file};
    proposals_.erase(proposal_id);
    applied_[proposal_id] = applied;
    return applied;
}

RolledBackCodeChange SafeSelfEditor::rollback(std::uint64_t proposal_id,
                                              std::string_view confirmation) {
    // Assim como apply(), rollback permanece exclusivo até a publicação atômica.
    std::lock_guard lock(mutex_);
    const std::string expected = "REVERTER " + std::to_string(proposal_id);
    if (confirmation != expected) {
        throw Error("confirmação inválida; digite exatamente: " + expected);
    }

    const std::filesystem::path backup_directory = root_ / ".cortex-backups";
    const std::filesystem::path manifest = rollback_manifest(backup_directory, proposal_id);
    AppliedCodeChange applied;
    const auto found = applied_.find(proposal_id);
    if (found != applied_.end()) {
        applied = found->second;
    } else {
        // Depois de recompilar/reiniciar, o mapa em RAM desaparece; o manifesto
        // restrito ao diretório de backups preserva o rollback verificável.
        const RollbackMetadata metadata = read_rollback_metadata(manifest);
        const std::filesystem::path target = resolve_target(metadata.target_path, true);
        std::filesystem::path backup;
        if (!metadata.created_file) {
            const std::filesystem::path backup_input = backup_directory / metadata.backup_name;
            backup = std::filesystem::weakly_canonical(backup_input);
            if (!path_begins_with(backup, std::filesystem::weakly_canonical(backup_directory))
                || !std::filesystem::is_regular_file(backup)
                || std::filesystem::is_symlink(backup_input)) {
                throw Error("backup persistente ausente, simbólico ou fora do diretório permitido");
            }
        }
        applied = AppliedCodeChange{
            proposal_id, target, backup, metadata.applied_hash, metadata.created_file};
    }
    const std::string target_request = unrestricted_scope_
        ? applied.changed_file.string()
        : std::filesystem::relative(applied.changed_file, root_).string();
    const std::filesystem::path target = resolve_target(target_request, true);
    const std::string current = read_source(
        target, unrestricted_scope_ ? maximum_unrestricted_file_bytes : maximum_source_bytes);
    if (content_hash(current) != applied.applied_hash) {
        throw Error("arquivo mudou após a aplicação; rollback automático foi bloqueado");
    }
    if (applied.created_file) {
        if (!std::filesystem::remove(target)) {
            throw Error("não foi possível remover o arquivo criado durante rollback");
        }
    } else {
        const std::string original = read_source(
            applied.backup_file, maximum_unrestricted_file_bytes);
        const std::filesystem::path temporary = temporary_path_for(target);
        try {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(original.data(), static_cast<std::streamsize>(original.size()));
            stream.flush();
            if (!stream) throw Error("falha ao restaurar backup da autoedição");
            stream.close();
            replace_file(temporary, target);
        } catch (...) {
            std::filesystem::remove(temporary);
            throw;
        }
    }
    applied_.erase(proposal_id);
    std::error_code cleanup_error;
    std::filesystem::remove(manifest, cleanup_error);
    return RolledBackCodeChange{proposal_id, target, applied.backup_file};
}

ValidationProposal SafeSelfEditor::stage_validation(std::string_view command) {
    if (!enabled_ || !unrestricted_scope_) {
        throw Error("validação livre requer --unrestricted-self-edit");
    }
    if (command.empty() || command.size() > 16U * 1024U
        || command.find('\0') != std::string_view::npos) {
        throw Error("comando de validação vazio, binário ou grande demais");
    }
    std::lock_guard lock(mutex_);
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw Error("espaço de IDs de autorização esgotado");
    }
    ValidationProposal proposal{next_id_++, std::string(command)};
    validations_[proposal.id] = proposal;
    return proposal;
}

ValidationResult SafeSelfEditor::run_validation(std::uint64_t validation_id,
                                                 std::string_view confirmation) {
    ValidationProposal proposal;
    {
        std::lock_guard lock(mutex_);
        const auto found = validations_.find(validation_id);
        if (found == validations_.end()) {
            throw Error("validação não encontrada ou autorização já consumida");
        }
        const std::string expected = "EXECUTAR " + std::to_string(validation_id);
        if (confirmation != expected) {
            throw Error("confirmação inválida; digite exatamente: " + expected);
        }
        proposal = found->second;
        validations_.erase(found); // A autorização é de uso único, inclusive se o comando falhar.
    }

    const std::filesystem::path output_directory = root_ / ".cortex-validation";
    std::filesystem::create_directories(output_directory);
#ifdef _WIN32
    const std::filesystem::path script = output_directory
        / (std::to_string(validation_id) + ".cmd");
#else
    const std::filesystem::path script = output_directory
        / (std::to_string(validation_id) + ".sh");
#endif
    const std::filesystem::path log = output_directory
        / (std::to_string(validation_id) + ".log");
    {
        std::ofstream output(script, std::ios::binary | std::ios::trunc);
#ifdef _WIN32
        output << "@echo off\r\n" << proposal.command << "\r\n";
#else
        output << proposal.command << '\n';
#endif
        if (!output) throw Error("não foi possível preparar a validação autorizada");
    }

    std::string dispatch;
#ifdef _WIN32
    dispatch = "cd /D \"" + root_.string() + "\" && call \""
               + script.string() + "\" > \"" + log.string() + "\" 2>&1";
#else
    const auto quote = [](std::string_view value) {
        std::string result{"'"};
        for (const char character : value) {
            if (character == '\'') result += "'\\''";
            else result.push_back(character);
        }
        result.push_back('\'');
        return result;
    };
    dispatch = "cd " + quote(root_.string()) + " && /bin/sh " + quote(script.string())
               + " > " + quote(log.string()) + " 2>&1";
#endif
    const int raw_status = std::system(dispatch.c_str());
    std::error_code cleanup_error;
    std::filesystem::remove(script, cleanup_error);

    int exit_code = raw_status;
#ifndef _WIN32
    if (raw_status != -1) {
        exit_code = WIFEXITED(raw_status) ? WEXITSTATUS(raw_status)
                    : WIFSIGNALED(raw_status) ? 128 + WTERMSIG(raw_status) : raw_status;
    }
#endif
    std::string captured;
    std::ifstream input(log, std::ios::binary);
    if (input) {
        constexpr std::size_t maximum_report_bytes = 1U * 1024U * 1024U;
        captured.resize(maximum_report_bytes);
        input.read(captured.data(), static_cast<std::streamsize>(captured.size()));
        captured.resize(static_cast<std::size_t>(input.gcount()));
        if (input.peek() != std::char_traits<char>::eof()) {
            captured += "\n[saída truncada na interface; consulte o log completo]";
        }
    }
    return ValidationResult{validation_id, exit_code, std::move(captured), log};
}

} // namespace cortex
