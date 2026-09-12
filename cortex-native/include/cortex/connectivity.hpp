#pragma once

#include "cortex/core.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cortex {

// Resposta de transporte independente do provedor. TLS fica no curl/WinHTTP nativo.
struct NativeHttpResponse final {
    long status = 0;
    std::string body;
};

class NativeHttpClient final {
public:
    explicit NativeHttpClient(std::string executable = {});

    [[nodiscard]] NativeHttpResponse get(std::string_view url,
                                         std::size_t timeout_seconds = 12U,
                                         std::size_t maximum_response_bytes = 2U * 1024U * 1024U) const;
    [[nodiscard]] NativeHttpResponse post_json(
        std::string_view url,
        std::span<const std::pair<std::string, std::string>> headers,
        std::string_view json,
        std::size_t timeout_seconds = 30U,
        std::size_t maximum_response_bytes = 2U * 1024U * 1024U) const;

    // HTTPS é obrigatório fora da máquina local para impedir envio de tokens em texto claro.
    static void validate_url(std::string_view url);

private:
    [[nodiscard]] NativeHttpResponse perform(
        std::string_view method,
        std::string_view url,
        std::span<const std::pair<std::string, std::string>> headers,
        std::string_view body,
        std::size_t timeout_seconds,
        std::size_t maximum_response_bytes) const;

    std::string executable_;
};

enum class LanguageReasoningMode : std::uint8_t {
    disabled,
    low_effort,
    full
};

struct LanguageApiConfig final {
    // Endpoint compatível com POST /v1/chat/completions (OpenAI, Groq, llama.cpp etc.).
    std::string endpoint;
    std::string model;
    // Metadados públicos para status/UI; não alteram o protocolo OpenAI-compatible.
    std::string provider = "openai-compatible";
    std::string display_model;
    std::uint64_t parameter_count = 0U;
    std::size_t context_tokens = 0U; // zero quando o provedor gerencia o limite.
    // O segredo é lido desta variável e nunca é aceito pela interface web.
    std::string api_key_environment = "CORTEX_LLM_API_KEY";
    // Campo OpenAI-compatible opcional; Gemini aceita low/medium/high.
    std::string reasoning_effort;
    // Alguns servidores locais aceitam amostragem e raciocínio por campos JSON adicionais.
    bool extended_sampling = false;
    // Modelos Granite 4.2 usam chat_template_kwargs, sem diretivas inseridas no texto.
    bool supports_reasoning_control = false;
    bool requires_reasoning_sampling = false; // Granite 4.2 requer temperature=1, top_p=0.95.
    float top_p = 0.8F;
    float presence_penalty = 1.5F;
    std::size_t timeout_seconds = 30U;
};

struct LanguageCompletion final {
    std::string text;
    std::string model;
};

class LanguageApiClient final {
public:
    explicit LanguageApiClient(LanguageApiConfig config,
                               NativeHttpClient http = NativeHttpClient{});

    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] const LanguageApiConfig& config() const noexcept { return config_; }
    [[nodiscard]] LanguageCompletion complete(std::string_view system_prompt,
                                               std::string_view user_prompt,
                                               float temperature,
                                               std::size_t maximum_tokens,
                                               std::size_t top_k = 20U,
                                               LanguageReasoningMode reasoning_mode
                                                   = LanguageReasoningMode::disabled) const;

private:
    LanguageApiConfig config_;
    NativeHttpClient http_;
    std::string api_key_;
};

struct InternetResult final {
    std::string heading;
    std::string summary;
    std::string source_url;
    std::string source_name;

    [[nodiscard]] bool empty() const noexcept { return summary.empty(); }
};

// Pesquisa sem chave: consulta DuckDuckGo e Wikipedia em português e preserva as fontes.
class InternetSearch final {
public:
    explicit InternetSearch(NativeHttpClient http = NativeHttpClient{});
    [[nodiscard]] InternetResult search(std::string_view query) const;
    [[nodiscard]] std::vector<InternetResult> search_multiple(
        std::string_view query, std::size_t maximum_results = 3U) const;

private:
    NativeHttpClient http_;
};

// Utilitários expostos para testes determinísticos do protocolo, sem chamadas externas.
[[nodiscard]] std::string url_encode(std::string_view value);
[[nodiscard]] std::optional<std::string> json_string_field(std::string_view json,
                                                            std::string_view field,
                                                            std::size_t start_offset = 0U);
[[nodiscard]] std::string json_quote(std::string_view value);

} // namespace cortex
