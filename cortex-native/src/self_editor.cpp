#include "cortex/self_editor.hpp"

#include "cortex/core.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

namespace cortex {
namespace {

constexpr std::size_t maximum_source_bytes = 192U * 1024U;
constexpr std::size_t maximum_instruction_bytes = 4U * 1024U;
constexpr std::size_t maximum_replacement_bytes = 256U * 1024U;

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

} // namespace

SafeSelfEditor::SafeSelfEditor(std::filesystem::path source_root,
                               const LanguageApiClient& language_api,
                               bool enabled)
    : language_api_(language_api), enabled_(enabled) {
    if (!source_root.empty()) {
        root_ = std::filesystem::weakly_canonical(std::move(source_root));
    }
    if (enabled_ && (root_.empty() || !std::filesystem::is_directory(root_))) {
        throw Error("--source-root não aponta para o projeto Cortex");
    }
}

std::filesystem::path SafeSelfEditor::resolve_allowed(std::string_view relative_path) const {
    if (!enabled_) throw Error("autoedição está desativada; use --enable-self-edit");
    if (relative_path.empty() || relative_path.size() > 512U) {
        throw Error("caminho de autoedição vazio ou longo demais");
    }
    const std::filesystem::path relative{std::string(relative_path)};
    if (relative.is_absolute() || !allowed_top_directory(relative)
        || !allowed_extension(relative)) {
        throw Error("autoedição aceita somente fontes em src/include/tests/data");
    }
    for (const auto& component : relative) {
        if (component == ".." || component == ".") {
            throw Error("componentes relativos não são aceitos na autoedição");
        }
    }

    const std::filesystem::path resolved = std::filesystem::weakly_canonical(root_ / relative);
    if (!path_begins_with(resolved, root_) || !std::filesystem::is_regular_file(resolved)
        || std::filesystem::is_symlink(root_ / relative)) {
        throw Error("arquivo de autoedição ausente, simbólico ou fora do projeto");
    }
    return resolved;
}

std::string SafeSelfEditor::read_source(const std::filesystem::path& path) {
    const std::uintmax_t size = std::filesystem::file_size(path);
    if (size == 0U || size > maximum_source_bytes) {
        throw Error("arquivo-fonte vazio ou grande demais para autoedição");
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

std::string SafeSelfEditor::extract_replacement(std::string_view completion) {
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
    if (replacement.empty() || replacement.size() > maximum_replacement_bytes
        || replacement.find('\0') != std::string::npos) {
        throw Error("API devolveu substituição vazia, binária ou grande demais");
    }
    replacement.push_back('\n');
    return replacement;
}

CodeProposal SafeSelfEditor::propose(std::string_view relative_path,
                                     std::string_view instruction) {
    if (!language_api_.configured()) {
        throw Error("autoedição requer uma API de linguagem configurada");
    }
    if (instruction.empty() || instruction.size() > maximum_instruction_bytes) {
        throw Error("instrução de autoedição vazia ou grande demais");
    }
    const std::filesystem::path resolved = resolve_allowed(relative_path);
    const std::string original = read_source(resolved);

    const std::string system =
        "Você é um engenheiro C++20. Modifique somente o arquivo recebido. "
        "Preserve as APIs não relacionadas, não adicione Python ou Node.js, não execute comandos "
        "e devolva o conteúdo COMPLETO do arquivo em um único bloco de código, sem explicações.";
    std::ostringstream user;
    user << "ARQUIVO: " << relative_path << "\nINSTRUÇÃO: " << instruction
         << "\n\nCONTEÚDO ATUAL:\n" << original;
    const LanguageCompletion completion = language_api_.complete(system, user.str(), 0.2F, 8'192U);

    CodeProposal proposal;
    {
        std::lock_guard lock(mutex_);
        proposal.id = next_id_++;
        proposal.relative_path = std::string(relative_path);
        proposal.instruction = std::string(instruction);
        proposal.replacement = extract_replacement(completion.text);
        proposal.original_hash = content_hash(original);
        proposals_[proposal.id] = proposal;
    }
    return proposal;
}

AppliedCodeChange SafeSelfEditor::apply(std::uint64_t proposal_id,
                                        std::string_view confirmation) {
    CodeProposal proposal;
    {
        std::lock_guard lock(mutex_);
        const auto found = proposals_.find(proposal_id);
        if (found == proposals_.end()) throw Error("proposta de código não encontrada ou expirada");
        proposal = found->second;
    }
    const std::string expected = "APLICAR " + std::to_string(proposal_id);
    if (confirmation != expected) {
        throw Error("confirmação inválida; digite exatamente: " + expected);
    }

    const std::filesystem::path target = resolve_allowed(proposal.relative_path);
    const std::string current = read_source(target);
    if (content_hash(current) != proposal.original_hash) {
        throw Error("o arquivo mudou após a proposta; gere outra para evitar conflito");
    }

    const std::filesystem::path backup_directory = root_ / ".cortex-backups";
    std::filesystem::create_directories(backup_directory);
    std::filesystem::path backup = backup_directory
        / (std::to_string(proposal.id) + "-" + target.filename().string());
    for (std::size_t suffix = 1U; std::filesystem::exists(backup); ++suffix) {
        backup = backup_directory / (std::to_string(proposal.id) + "-"
                 + std::to_string(suffix) + "-" + target.filename().string());
    }
    {
        std::ofstream stream(backup, std::ios::binary | std::ios::trunc);
        stream.write(current.data(), static_cast<std::streamsize>(current.size()));
        if (!stream) throw Error("não foi possível criar backup da autoedição");
    }

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
        throw;
    }

    {
        std::lock_guard lock(mutex_);
        proposals_.erase(proposal_id);
    }
    return AppliedCodeChange{proposal_id, target, backup};
}

} // namespace cortex
