#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cortex {

// Perfil de um modelo aberto já treinado. Os pesos GGUF são baixados pelo
// llama.cpp para o cache do usuário e nunca são adicionados ao repositório.
struct OpenModelProfile final {
    std::string id;
    std::string display_name;
    std::string hugging_face_reference;
    std::uint64_t parameter_count = 0U;
    std::size_t minimum_ram_gib = 0U;
    std::size_t recommended_context = 4'096U;
    std::size_t recommended_threads = 0U;
    int recommended_gpu_layers = 999;
    bool supports_reasoning_control = false;
};

// Perfis revisados para diferentes máquinas; lança exceção se o nome for inválido.
[[nodiscard]] OpenModelProfile open_model_profile(const std::string& id);
[[nodiscard]] std::vector<OpenModelProfile> available_open_model_profiles();
[[nodiscard]] std::size_t system_memory_gib() noexcept;
[[nodiscard]] OpenModelProfile recommended_open_model_profile(std::size_t memory_gib);

struct LocalModelOptions final {
    std::string executable = "llama-server";
    std::filesystem::path model_path;
    std::string hugging_face_reference;
    std::string alias = "cortex-local";
    std::uint16_t port = 8'081U;
    std::size_t context_tokens = 4'096U;
    std::size_t threads = 0U; // zero deixa o llama.cpp escolher.
    int gpu_layers = 999;     // tenta offload total; backend CPU ignora se não houver GPU.
    std::size_t startup_timeout_seconds = 1'200U;
};

// Argumentos são expostos para teste determinístico e auditoria de segurança.
[[nodiscard]] std::vector<std::string> local_model_arguments(
    const LocalModelOptions& options);

// Processo RAII do llama-server. Não usa shell, Python, Node ou Docker.
class LocalModelServer final {
public:
    explicit LocalModelServer(LocalModelOptions options);
    ~LocalModelServer();

    LocalModelServer(const LocalModelServer&) = delete;
    LocalModelServer& operator=(const LocalModelServer&) = delete;
    LocalModelServer(LocalModelServer&&) = delete;
    LocalModelServer& operator=(LocalModelServer&&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const LocalModelOptions& options() const noexcept { return options_; }
    [[nodiscard]] std::string endpoint() const;

private:
    [[nodiscard]] bool health_ready() const noexcept;
    [[nodiscard]] bool child_alive() noexcept;

    LocalModelOptions options_;
    bool adopted_existing_server_ = false;
#ifdef _WIN32
    void* process_handle_ = nullptr;
    void* thread_handle_ = nullptr;
#else
    int process_id_ = -1;
#endif
};

} // namespace cortex
