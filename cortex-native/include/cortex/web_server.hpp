#pragma once

#include "cortex/agent.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace cortex {

// Configuração do servidor HTTP nativo que hospeda a interface de conversa.
struct WebServerOptions final {
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 8080U;
    std::size_t worker_count = 4U;
    std::size_t maximum_request_bytes = 64U * 1024U;
    std::filesystem::path memory_path;
    AgentOptions agent;
};

// HTML/CSS/JavaScript são embutidos no binário; não existe Node.js nem build web.
[[nodiscard]] std::string_view web_chat_html() noexcept;

// Bloqueia a thread chamadora e atende a UI/API até o processo ser encerrado.
void run_web_chat_server(const BpeTokenizer& tokenizer,
                         const NeuralLanguageModel& model,
                         LongTermMemory& memory,
                         const WebServerOptions& options);

} // namespace cortex
