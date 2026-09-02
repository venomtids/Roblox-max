#pragma once

#include "cortex/connectivity.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cortex {

struct SourceAudit final {
    std::string relative_path;
    std::size_t bytes = 0U;
    std::size_t lines = 0U;
    std::size_t long_lines = 0U;
    std::size_t todo_markers = 0U;
    std::size_t risky_calls = 0U;
    double maintainability_score = 0.0;
    std::vector<std::string> findings;
    std::string suggested_instruction;
};

struct CodeProposal final {
    std::uint64_t id = 0U;
    std::string relative_path;
    std::string instruction;
    std::string replacement;
    std::uint64_t original_hash = 0U;
    std::string audit_summary;
    std::string risk_level = "medium";
    bool creates_file = false;
    bool unrestricted_scope = false;
};

struct AppliedCodeChange final {
    std::uint64_t proposal_id = 0U;
    std::filesystem::path changed_file;
    std::filesystem::path backup_file;
    std::uint64_t applied_hash = 0U;
    bool created_file = false;
};

struct RolledBackCodeChange final {
    std::uint64_t proposal_id = 0U;
    std::filesystem::path restored_file;
    std::filesystem::path backup_file;
};

struct ValidationProposal final {
    std::uint64_t id = 0U;
    std::string command;
};

struct ValidationResult final {
    std::uint64_t id = 0U;
    int exit_code = -1;
    std::string output;
    std::filesystem::path full_log;
};

// Autoedição supervisionada. O modo padrão mantém a allowlist; o modo irrestrito
// aceita arquivos existentes ou novos em qualquer caminho autorizado pelo sistema
// operacional, mas cada escrita continua exigindo consentimento humano explícito.
class SafeSelfEditor final {
public:
    SafeSelfEditor(std::filesystem::path source_root,
                   const LanguageApiClient& language_api,
                   bool enabled,
                   bool unrestricted_scope = false);

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool unrestricted_scope() const noexcept { return unrestricted_scope_; }
    [[nodiscard]] SourceAudit audit(std::string_view relative_path,
                                    std::string_view observed_behavior = {}) const;
    [[nodiscard]] CodeProposal propose(std::string_view relative_path,
                                       std::string_view instruction);
    [[nodiscard]] CodeProposal propose_file(std::string_view path,
                                            std::string_view instruction,
                                            bool create_if_missing);
    [[nodiscard]] CodeProposal propose_refinement(std::string_view relative_path,
                                                  std::string_view objective,
                                                  std::string_view observed_behavior = {});
    [[nodiscard]] AppliedCodeChange apply(std::uint64_t proposal_id,
                                          std::string_view confirmation);
    [[nodiscard]] RolledBackCodeChange rollback(std::uint64_t proposal_id,
                                                std::string_view confirmation);
    [[nodiscard]] ValidationProposal stage_validation(std::string_view command);
    [[nodiscard]] ValidationResult run_validation(std::uint64_t validation_id,
                                                  std::string_view confirmation);

private:
    [[nodiscard]] std::filesystem::path resolve_target(std::string_view path,
                                                       bool require_existing) const;
    [[nodiscard]] static std::string read_source(const std::filesystem::path& path,
                                                 std::size_t maximum_bytes);
    [[nodiscard]] static std::uint64_t content_hash(std::string_view content) noexcept;
    [[nodiscard]] static std::string extract_replacement(std::string_view completion,
                                                         std::size_t maximum_bytes);
    static void validate_replacement(const std::filesystem::path& path,
                                     std::string_view original,
                                     std::string_view replacement);

    std::filesystem::path root_;
    const LanguageApiClient& language_api_;
    bool enabled_ = false;
    bool unrestricted_scope_ = false;
    std::uint64_t next_id_ = 1U;
    std::unordered_map<std::uint64_t, CodeProposal> proposals_;
    std::unordered_map<std::uint64_t, AppliedCodeChange> applied_;
    std::unordered_map<std::uint64_t, ValidationProposal> validations_;
    mutable std::mutex mutex_;
};

} // namespace cortex
