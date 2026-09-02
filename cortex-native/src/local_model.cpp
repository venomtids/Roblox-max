#include "cortex/local_model.hpp"

#include "cortex/connectivity.hpp"
#include "cortex/core.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cortex {
namespace {

[[nodiscard]] bool safe_text(std::string_view text, std::size_t maximum) {
    return !text.empty() && text.size() <= maximum
           && text.find_first_of("\r\n") == std::string_view::npos
           && text.find('\0') == std::string_view::npos;
}

void validate_options(const LocalModelOptions& options) {
    if (!safe_text(options.executable, 4'096U) || !safe_text(options.alias, 128U)) {
        throw Error("executável ou alias inválido para modelo local");
    }
    const bool has_path = !options.model_path.empty();
    const bool has_hugging_face = !options.hugging_face_reference.empty();
    if (has_path == has_hugging_face) {
        throw Error("informe exatamente um entre --local-model e --local-hf");
    }
    if (has_path) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(options.model_path, error)) {
            throw Error("arquivo GGUF local não encontrado: " + options.model_path.string());
        }
    } else if (!safe_text(options.hugging_face_reference, 512U)
               || options.hugging_face_reference.find_first_not_of(
                      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._/:@")
                      != std::string::npos) {
        throw Error("referência Hugging Face inválida para modelo local");
    }
    if (options.port == 0U || options.context_tokens < 512U
        || options.context_tokens > 131'072U || options.threads > 1'024U
        || options.gpu_layers < -1 || options.gpu_layers > 9'999
        || options.startup_timeout_seconds < 5U
        || options.startup_timeout_seconds > 7'200U) {
        throw Error("limites inválidos para servidor de modelo local");
    }
}

#ifdef _WIN32
[[nodiscard]] std::string quote_windows_argument(std::string_view argument) {
    if (argument.find_first_of(" \t\"") == std::string_view::npos) {
        return std::string(argument);
    }
    std::string result = "\"";
    std::size_t backslashes = 0U;
    for (const char character : argument) {
        if (character == '\\') {
            ++backslashes;
        } else if (character == '"') {
            result.append(backslashes * 2U + 1U, '\\');
            result.push_back('"');
            backslashes = 0U;
        } else {
            result.append(backslashes, '\\');
            backslashes = 0U;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2U, '\\');
    result.push_back('"');
    return result;
}
#endif

} // namespace

std::vector<OpenModelProfile> available_open_model_profiles() {
    return {
        {"tiny", "Qwen3 0.6B Q8", "Qwen/Qwen3-0.6B-GGUF:Q8_0",
         600'000'000ULL, 2U, 4'096U},
        {"edge", "Qwen3 1.7B Q8", "Qwen/Qwen3-1.7B-GGUF:Q8_0",
         1'700'000'000ULL, 4U, 4'096U},
        {"daily", "Qwen3 4B Q4_K_M", "Qwen/Qwen3-4B-GGUF:Q4_K_M",
         4'000'000'000ULL, 8U, 8'192U},
        {"workstation", "Qwen3 14B Q4_K_M", "Qwen/Qwen3-14B-GGUF:Q4_K_M",
         14'800'000'000ULL, 24U, 16'384U},
        {"max", "Qwen3 30B-A3B Q4_K_M", "Qwen/Qwen3-30B-A3B-GGUF:Q4_K_M",
         30'500'000'000ULL, 32U, 16'384U}
    };
}

std::size_t system_memory_gib() noexcept {
    constexpr std::uint64_t gibibyte = 1'073'741'824ULL;
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return 0U;
    return static_cast<std::size_t>((memory.ullTotalPhys + gibibyte - 1U) / gibibyte);
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0L || page_size <= 0L) return 0U;
    const auto bytes = static_cast<std::uint64_t>(pages)
                       * static_cast<std::uint64_t>(page_size);
    return static_cast<std::size_t>((bytes + gibibyte - 1U) / gibibyte);
#endif
}

OpenModelProfile recommended_open_model_profile(std::size_t memory_gib) {
    if (memory_gib >= 32U) return open_model_profile("max");
    if (memory_gib >= 24U) return open_model_profile("workstation");
    if (memory_gib >= 8U) return open_model_profile("daily");
    if (memory_gib >= 4U) return open_model_profile("edge");
    return open_model_profile("tiny");
}

OpenModelProfile open_model_profile(const std::string& id) {
    const std::vector<OpenModelProfile> profiles = available_open_model_profiles();
    const auto found = std::find_if(profiles.begin(), profiles.end(), [&](const auto& profile) {
        return profile.id == id;
    });
    if (found == profiles.end()) {
        throw Error("perfil local desconhecido: " + id
                    + " (use tiny, edge, daily, workstation ou max)");
    }
    return *found;
}

std::vector<std::string> local_model_arguments(const LocalModelOptions& options) {
    validate_options(options);
    std::vector<std::string> arguments{options.executable};
    if (!options.model_path.empty()) {
        arguments.emplace_back("-m");
        arguments.push_back(options.model_path.string());
    } else {
        arguments.emplace_back("-hf");
        arguments.push_back(options.hugging_face_reference);
    }
    arguments.insert(arguments.end(), {
        "--host", "127.0.0.1",
        "--port", std::to_string(options.port),
        "--alias", options.alias,
        "--jinja",
        "-c", std::to_string(options.context_tokens)
    });
    if (options.threads != 0U) {
        arguments.emplace_back("-t");
        arguments.push_back(std::to_string(options.threads));
    }
    if (options.gpu_layers >= 0) {
        arguments.emplace_back("-ngl");
        arguments.push_back(std::to_string(options.gpu_layers));
    }
    return arguments;
}

LocalModelServer::LocalModelServer(LocalModelOptions options)
    : options_(std::move(options)) {
    validate_options(options_);
}

LocalModelServer::~LocalModelServer() {
    stop();
}

std::string LocalModelServer::endpoint() const {
    return "http://127.0.0.1:" + std::to_string(options_.port)
           + "/v1/chat/completions";
}

bool LocalModelServer::health_ready() const noexcept {
    try {
        NativeHttpClient http;
        const std::string base = "http://127.0.0.1:" + std::to_string(options_.port);
        const NativeHttpResponse health = http.get(base + "/health", 2U, 64U * 1024U);
        if (health.status != 200L) return false;
        const NativeHttpResponse models = http.get(base + "/v1/models", 2U, 256U * 1024U);
        return models.status == 200L
               && models.body.find(options_.alias) != std::string::npos;
    } catch (...) {
        return false;
    }
}

bool LocalModelServer::child_alive() noexcept {
    if (adopted_existing_server_) return true;
#ifdef _WIN32
    if (process_handle_ == nullptr) return false;
    DWORD exit_code = 0U;
    if (!GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &exit_code)) return false;
    return exit_code == STILL_ACTIVE;
#else
    if (process_id_ < 0) return false;
    int status = 0;
    const pid_t result = waitpid(static_cast<pid_t>(process_id_), &status, WNOHANG);
    if (result == 0) return true;
    if (result == static_cast<pid_t>(process_id_)) process_id_ = -1;
    return false;
#endif
}

bool LocalModelServer::running() const noexcept {
    if (adopted_existing_server_) return health_ready();
#ifdef _WIN32
    if (process_handle_ == nullptr) return false;
    DWORD exit_code = 0U;
    return GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &exit_code)
           && exit_code == STILL_ACTIVE;
#else
    if (process_id_ < 0) return false;
    return kill(static_cast<pid_t>(process_id_), 0) == 0 || errno == EPERM;
#endif
}

void LocalModelServer::start() {
    if (running()) return;
    if (health_ready()) {
        adopted_existing_server_ = true;
        std::cout << "Modelo local já ativo em " << endpoint() << '\n';
        return;
    }

    const std::vector<std::string> arguments = local_model_arguments(options_);
    std::cout << "Iniciando modelo aberto local: " << options_.alias << '\n'
              << "Backend: llama.cpp nativo | endpoint: " << endpoint() << '\n'
              << std::flush;

#ifdef _WIN32
    std::string command_line;
    for (const std::string& argument : arguments) {
        if (!command_line.empty()) command_line.push_back(' ');
        command_line += quote_windows_argument(argument);
    }
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                                        FALSE, 0U, nullptr, nullptr, &startup, &process);
    if (!created) {
        throw Error("não foi possível iniciar llama-server; instale com `winget install -e --id ggml.llamacpp`");
    }
    process_handle_ = process.hProcess;
    thread_handle_ = process.hThread;
#else
    const pid_t child = fork();
    if (child < 0) throw Error("fork falhou ao iniciar llama-server");
    if (child == 0) {
        std::vector<char*> native_arguments;
        native_arguments.reserve(arguments.size() + 1U);
        for (const std::string& argument : arguments) {
            native_arguments.push_back(const_cast<char*>(argument.c_str()));
        }
        native_arguments.push_back(nullptr);
        execvp(native_arguments.front(), native_arguments.data());
        std::cerr << "erro: llama-server não encontrado; instale o binário C++ do llama.cpp\n";
        _exit(127);
    }
    process_id_ = static_cast<int>(child);
#endif

    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(options_.startup_timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!child_alive()) {
            stop();
            throw Error("llama-server encerrou durante a carga; confira instalação, GGUF e RAM");
        }
        if (health_ready()) {
            std::cout << "Modelo aberto pronto: " << options_.alias << '\n';
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    stop();
    throw Error("llama-server não ficou pronto dentro do timeout configurado");
}

void LocalModelServer::stop() noexcept {
    if (adopted_existing_server_) {
        adopted_existing_server_ = false;
        return;
    }
#ifdef _WIN32
    if (process_handle_ != nullptr) {
        HANDLE process = static_cast<HANDLE>(process_handle_);
        DWORD exit_code = 0U;
        if (GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE) {
            static_cast<void>(TerminateProcess(process, 0U));
            static_cast<void>(WaitForSingleObject(process, 3'000U));
        }
        CloseHandle(process);
        process_handle_ = nullptr;
    }
    if (thread_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(thread_handle_));
        thread_handle_ = nullptr;
    }
#else
    if (process_id_ < 0) return;
    const pid_t child = static_cast<pid_t>(process_id_);
    static_cast<void>(kill(child, SIGTERM));
    for (std::size_t attempt = 0U; attempt < 30U; ++attempt) {
        int status = 0;
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) {
            process_id_ = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    static_cast<void>(kill(child, SIGKILL));
    static_cast<void>(waitpid(child, nullptr, 0));
    process_id_ = -1;
#endif
}

} // namespace cortex
