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
    if (!language_api_.configured()) {
        throw Error("autoedição requer um modelo GGUF local ativo ou provedor opt-in");
    }
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

    const std::string system = unrestricted_scope_
        ? "Você é o motor de evolução supervisionada da Cortex. O usuário autorizou propor a criação "
          "ou substituição completa deste arquivo. Não use Python, Node.js ou npm. Devolva somente o "
          "conteúdo COMPLETO em um único bloco de código; a proposta ainda será revisada pelo usuário."
        : "Você é um engenheiro C++20. Modifique somente o arquivo recebido. Preserve as APIs não "
          "relacionadas, não adicione Python ou Node.js, não execute comandos e devolva o conteúdo "
          "COMPLETO do arquivo em um único bloco de código, sem explicações.";
    std::ostringstream user;
    user << "ARQUIVO: " << resolved.string() << "\nOPERAÇÃO: "
         << (creates_file ? "CRIAR" : "SUBSTITUIR") << "\nINSTRUÇÃO: " << instruction
         << "\n\nCONTEÚDO ATUAL:\n"
         << (creates_file ? std::string("[ARQUIVO AINDA NÃO EXISTE]") : original);
    const LanguageCompletion completion = language_api_.complete(system, user.str(), 0.2F, 8'192U);
    std::string replacement = extract_replacement(
        completion.text, unrestricted_scope_ ? maximum_unrestricted_replacement_bytes
                                             : maximum_replacement_bytes);
    if (unrestricted_scope_) {
        if (replacement.empty() || replacement.size() > maximum_unrestricted_replacement_bytes
            || replacement == original) {
            throw Error("proposta irrestrita vazia, idêntica ou acima do limite operacional");
        }
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
