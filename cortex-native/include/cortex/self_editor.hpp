#pragma once

#include "cortex/connectivity.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cortex {

struct CodeProposal final {
    std::uint64_t id = 0U;
    std::string relative_path;
    std::string instruction;
    std::string replacement;
    std::uint64_t original_hash = 0U;
};

struct AppliedCodeChange final {
    std::uint64_t proposal_id = 0U;
    std::filesystem::path changed_file;
    std::filesystem::path backup_file;
};

// Autoedição deliberadamente controlada: só um arquivo permitido, proposta visível,
// confirmação humana exata, detecção de conflito e backup antes da escrita atômica.
class SafeSelfEditor final {
public:
    SafeSelfEditor(std::filesystem::path source_root,
                   const LanguageApiClient& language_api,
                   bool enabled);

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] CodeProposal propose(std::string_view relative_path,
                                       std::string_view instruction);
    [[nodiscard]] AppliedCodeChange apply(std::uint64_t proposal_id,
                                          std::string_view confirmation);

private:
    [[nodiscard]] std::filesystem::path resolve_allowed(std::string_view relative_path) const;
    [[nodiscard]] static std::string read_source(const std::filesystem::path& path);
    [[nodiscard]] static std::uint64_t content_hash(std::string_view content) noexcept;
    [[nodiscard]] static std::string extract_replacement(std::string_view completion);

    std::filesystem::path root_;
    const LanguageApiClient& language_api_;
    bool enabled_ = false;
    std::uint64_t next_id_ = 1U;
    std::unordered_map<std::uint64_t, CodeProposal> proposals_;
    std::mutex mutex_;
};

} // namespace cortex
