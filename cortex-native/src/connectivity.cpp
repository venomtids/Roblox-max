#include "cortex/connectivity.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cortex {
namespace {

constexpr std::string_view status_marker = "\n__CORTEX_HTTP_STATUS__:";

struct ProcessResult final {
    int exit_code = -1;
    std::string output;
};

[[nodiscard]] std::string abbreviated(std::string_view text, std::size_t limit = 600U) {
    if (text.size() <= limit) {
        return std::string(text);
    }
    return std::string(text.substr(0U, limit)) + "…";
}

[[nodiscard]] std::string without_private_reasoning(std::string text) {
    // Servidores novos normalmente separam reasoning_content de content. Esta limpeza
    // cobre runtimes antigos que ainda devolvem o bloco <think> no texto final.
    const std::size_t closing = text.rfind("</think>");
    if (closing != std::string::npos) {
        text.erase(0U, closing + std::string_view("</think>").size());
    }
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n'
                             || text.front() == '\r' || text.front() == '\t')) {
        text.erase(text.begin());
    }
    return text;
}

void truncate_utf8(std::string& text, std::size_t maximum_bytes) {
    if (text.size() <= maximum_bytes) return;
    std::size_t boundary = maximum_bytes;
    while (boundary > 0U
           && (static_cast<unsigned char>(text[boundary]) & 0xC0U) == 0x80U) {
        --boundary;
    }
    text.resize(boundary);
}

void limit_internet_result(InternetResult& result) {
    truncate_utf8(result.heading, 512U);
    truncate_utf8(result.summary, 8U * 1024U);
    truncate_utf8(result.source_url, 2U * 1024U);
    // URLs vindas do documento remoto são dados não confiáveis. Só exibimos e
    // persistimos uma referência HTTPS sintaticamente aceita pelo mesmo cliente.
    if (!result.source_url.empty()) {
        try {
            NativeHttpClient::validate_url(result.source_url);
            if (!result.source_url.starts_with("https://")) result.source_url.clear();
        } catch (const Error&) {
            result.source_url.clear();
        }
    }
}

// Valores sensíveis seguem pelo stdin de `curl --config -`, nunca pela linha de
// comando visível a outros processos. O quoting abaixo é o formato nativo do curl.
[[nodiscard]] std::string curl_config_quote(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2U);
    quoted.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': quoted += "\\\\"; break;
        case '"': quoted += "\\\""; break;
        case '\t': quoted += "\\t"; break;
        case '\n': quoted += "\\n"; break;
        case '\r': quoted += "\\r"; break;
        case '\v': quoted += "\\v"; break;
        default:
            if (character < 0x20U) {
                throw Error("JSON/cabeçalho contém controle incompatível com curl");
            }
            quoted.push_back(static_cast<char>(character));
        }
    }
    quoted.push_back('"');
    return quoted;
}

#ifndef _WIN32

[[nodiscard]] ProcessResult run_process(const std::vector<std::string>& arguments,
                                        std::string_view input,
                                        std::size_t timeout_seconds,
                                        std::size_t maximum_output) {
    if (arguments.empty()) {
        throw Error("processo nativo sem executável");
    }
    // curl pode encerrar antes de ler stdin; ignorar SIGPIPE converte isso em EPIPE.
    static std::once_flag sigpipe_once;
    std::call_once(sigpipe_once, []() { signal(SIGPIPE, SIG_IGN); });
    int input_pipe[2]{};
    int output_pipe[2]{};
    if (pipe(input_pipe) != 0) {
        throw Error("não foi possível criar pipe de entrada para HTTP");
    }
    if (pipe(output_pipe) != 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        throw Error("não foi possível criar pipe de saída para HTTP");
    }

    const pid_t child = fork();
    if (child < 0) {
        close(input_pipe[0]); close(input_pipe[1]);
        close(output_pipe[0]); close(output_pipe[1]);
        throw Error("fork falhou ao iniciar cliente HTTP");
    }
    if (child == 0) {
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(input_pipe[0]); close(input_pipe[1]);
        close(output_pipe[0]); close(output_pipe[1]);

        std::vector<char*> native_arguments;
        native_arguments.reserve(arguments.size() + 1U);
        for (const std::string& argument : arguments) {
            native_arguments.push_back(const_cast<char*>(argument.c_str()));
        }
        native_arguments.push_back(nullptr);
        execvp(native_arguments.front(), native_arguments.data());
        _exit(127);
    }

    close(input_pipe[0]);
    close(output_pipe[1]);
    std::size_t written = 0U;
    while (written < input.size()) {
        const ssize_t amount = write(input_pipe[1], input.data() + written, input.size() - written);
        if (amount > 0) {
            written += static_cast<std::size_t>(amount);
        } else if (amount < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(input_pipe[1]);

    const int flags = fcntl(output_pipe[0], F_GETFL, 0);
    if (flags >= 0) {
        static_cast<void>(fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK));
    }

    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeout_seconds + 3U);
    bool output_open = true;
    bool child_done = false;
    std::array<char, 8'192U> buffer{};

    while (output_open || !child_done) {
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, nullptr, 0);
            close(output_pipe[0]);
            throw Error("cliente HTTP excedeu o timeout");
        }
        if (output_open) {
            pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
            static_cast<void>(poll(&descriptor, 1, 50));
            while (true) {
                const ssize_t amount = read(output_pipe[0], buffer.data(), buffer.size());
                if (amount > 0) {
                    if (result.output.size() + static_cast<std::size_t>(amount)
                        > maximum_output + status_marker.size() + 64U) {
                        kill(child, SIGKILL);
                        waitpid(child, nullptr, 0);
                        close(output_pipe[0]);
                        throw Error("resposta HTTP excedeu o limite de memória");
                    }
                    result.output.append(buffer.data(), static_cast<std::size_t>(amount));
                } else if (amount == 0) {
                    output_open = false;
                    close(output_pipe[0]);
                    break;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    output_open = false;
                    close(output_pipe[0]);
                    break;
                }
            }
        }
        if (!child_done) {
            int status = 0;
            const pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) {
                child_done = true;
                result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
        }
        if (!output_open && !child_done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return result;
}

#else

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

[[nodiscard]] ProcessResult run_process(const std::vector<std::string>& arguments,
                                        std::string_view input,
                                        std::size_t timeout_seconds,
                                        std::size_t maximum_output) {
    if (arguments.empty()) {
        throw Error("processo nativo sem executável");
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE child_input_read = nullptr;
    HANDLE parent_input_write = nullptr;
    HANDLE parent_output_read = nullptr;
    HANDLE child_output_write = nullptr;
    if (!CreatePipe(&child_input_read, &parent_input_write, &security, 0)) {
        throw Error("não foi possível criar pipe Win32 de entrada para HTTP");
    }
    if (!CreatePipe(&parent_output_read, &child_output_write, &security, 0)) {
        CloseHandle(child_input_read);
        CloseHandle(parent_input_write);
        throw Error("não foi possível criar pipe Win32 de saída para HTTP");
    }
    SetHandleInformation(parent_input_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parent_output_read, HANDLE_FLAG_INHERIT, 0);

    std::string command_line;
    // O primeiro argumento é o executável: no Windows ele precisa de extensão
    // resolvida quando o processo foi iniciado por um shell estilo MSYS2.
    command_line = quote_windows_argument(resolve_executable_path(arguments.front()));
    for (std::size_t index = 1U; index < arguments.size(); ++index) {
        command_line.push_back(' ');
        command_line += quote_windows_argument(arguments[index]);
    }
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_input_read;
    startup.hStdOutput = child_output_write;
    startup.hStdError = child_output_write;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(child_input_read);
    CloseHandle(child_output_write);
    if (!created) {
        CloseHandle(parent_input_write);
        CloseHandle(parent_output_read);
        throw Error("não foi possível iniciar curl.exe");
    }

    std::size_t input_offset = 0U;
    while (input_offset < input.size()) {
        DWORD written = 0U;
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(
            input.size() - input_offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        if (!WriteFile(parent_input_write, input.data() + input_offset,
                       wanted, &written, nullptr) || written == 0U) {
            break;
        }
        input_offset += static_cast<std::size_t>(written);
    }
    CloseHandle(parent_input_write);

    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeout_seconds + 3U);
    std::array<char, 8'192U> buffer{};
    while (true) {
        DWORD available = 0U;
        if (PeekNamedPipe(parent_output_read, nullptr, 0, nullptr, &available, nullptr)
            && available != 0U) {
            DWORD read_amount = 0U;
            if (ReadFile(parent_output_read, buffer.data(),
                         static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available)),
                         &read_amount, nullptr) && read_amount != 0U) {
                result.output.append(buffer.data(), static_cast<std::size_t>(read_amount));
                if (result.output.size() > maximum_output + status_marker.size() + 64U) {
                    TerminateProcess(process.hProcess, 1U);
                    CloseHandle(parent_output_read);
                    CloseHandle(process.hThread);
                    CloseHandle(process.hProcess);
                    throw Error("resposta HTTP excedeu o limite de memória");
                }
            }
        }
        const DWORD wait = WaitForSingleObject(process.hProcess, 20U);
        if (wait == WAIT_OBJECT_0) {
            while (true) {
                DWORD read_amount = 0U;
                if (!ReadFile(parent_output_read, buffer.data(), static_cast<DWORD>(buffer.size()),
                              &read_amount, nullptr) || read_amount == 0U) break;
                result.output.append(buffer.data(), static_cast<std::size_t>(read_amount));
                if (result.output.size() > maximum_output + status_marker.size() + 64U) {
                    TerminateProcess(process.hProcess, 1U);
                    CloseHandle(parent_output_read);
                    CloseHandle(process.hThread);
                    CloseHandle(process.hProcess);
                    throw Error("resposta HTTP excedeu o limite de memória");
                }
            }
            DWORD exit_code = 1U;
            GetExitCodeProcess(process.hProcess, &exit_code);
            result.exit_code = static_cast<int>(exit_code);
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            TerminateProcess(process.hProcess, 1U);
            CloseHandle(parent_output_read);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            throw Error("cliente HTTP excedeu o timeout");
        }
    }
    CloseHandle(parent_output_read);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

#endif

[[nodiscard]] bool local_http_url(std::string_view url) {
    constexpr std::string_view prefix = "http://";
    if (!url.starts_with(prefix)) return false;
    const std::string_view authority = url.substr(prefix.size(), url.find('/', prefix.size())
                                                               - prefix.size());
    const std::size_t colon = authority.find(':');
    const std::string_view host = authority.substr(0U, colon);
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

void append_utf8(std::string& destination, std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
        destination.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        destination.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        destination.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        destination.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        destination.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        destination.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        destination.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        destination.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        destination.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        destination.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

[[nodiscard]] std::optional<std::string> parse_json_string_at(std::string_view json,
                                                               std::size_t quote_position) {
    if (quote_position >= json.size() || json[quote_position] != '"') return std::nullopt;
    std::string result;
    for (std::size_t position = quote_position + 1U; position < json.size();) {
        const unsigned char character = static_cast<unsigned char>(json[position++]);
        if (character == '"') return result;
        if (character < 0x20U) return std::nullopt;
        if (character != '\\') {
            result.push_back(static_cast<char>(character));
            continue;
        }
        if (position >= json.size()) return std::nullopt;
        const char escaped = json[position++];
        switch (escaped) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
            if (position + 4U > json.size()) return std::nullopt;
            std::uint32_t codepoint = 0U;
            for (std::size_t digit = 0U; digit < 4U; ++digit) {
                const char hex = json[position++];
                codepoint <<= 4U;
                if (hex >= '0' && hex <= '9') codepoint |= static_cast<std::uint32_t>(hex - '0');
                else if (hex >= 'a' && hex <= 'f') codepoint |= static_cast<std::uint32_t>(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F') codepoint |= static_cast<std::uint32_t>(hex - 'A' + 10);
                else return std::nullopt;
            }
            if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                if (position + 6U > json.size() || json[position] != '\\'
                    || json[position + 1U] != 'u') return std::nullopt;
                position += 2U;
                std::uint32_t low = 0U;
                for (std::size_t digit = 0U; digit < 4U; ++digit) {
                    const char hex = json[position++];
                    low <<= 4U;
                    if (hex >= '0' && hex <= '9') low |= static_cast<std::uint32_t>(hex - '0');
                    else if (hex >= 'a' && hex <= 'f') low |= static_cast<std::uint32_t>(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F') low |= static_cast<std::uint32_t>(hex - 'A' + 10);
                    else return std::nullopt;
                }
                if (low < 0xDC00U || low > 0xDFFFU) return std::nullopt;
                codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (low - 0xDC00U);
            }
            append_utf8(result, codepoint);
            break;
        }
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

std::string url_encode(std::string_view value) {
    static constexpr char hexadecimal[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3U);
    for (const unsigned char character : value) {
        const bool unreserved = (character >= 'A' && character <= 'Z')
                                || (character >= 'a' && character <= 'z')
                                || (character >= '0' && character <= '9')
                                || character == '-' || character == '_'
                                || character == '.' || character == '~';
        if (unreserved) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hexadecimal[character >> 4U]);
            result.push_back(hexadecimal[character & 0x0FU]);
        }
    }
    return result;
}

std::string json_quote(std::string_view value) {
    std::ostringstream result;
    result << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result << "\\\""; break;
        case '\\': result << "\\\\"; break;
        case '\b': result << "\\b"; break;
        case '\f': result << "\\f"; break;
        case '\n': result << "\\n"; break;
        case '\r': result << "\\r"; break;
        case '\t': result << "\\t"; break;
        default:
            if (character < 0x20U) {
                result << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(character) << std::dec;
            } else {
                result << static_cast<char>(character);
            }
        }
    }
    result << '"';
    return result.str();
}

std::optional<std::string> json_string_field(std::string_view json,
                                             std::string_view field,
                                             std::size_t start_offset) {
    const std::string needle = "\"" + std::string(field) + "\"";
    std::size_t position = start_offset;
    while ((position = json.find(needle, position)) != std::string_view::npos) {
        position += needle.size();
        while (position < json.size() && (json[position] == ' ' || json[position] == '\t'
                                          || json[position] == '\r' || json[position] == '\n')) {
            ++position;
        }
        if (position >= json.size() || json[position++] != ':') continue;
        while (position < json.size() && (json[position] == ' ' || json[position] == '\t'
                                          || json[position] == '\r' || json[position] == '\n')) {
            ++position;
        }
        if (position < json.size() && json[position] == '"') {
            if (auto parsed = parse_json_string_at(json, position)) return parsed;
        }
    }
    return std::nullopt;
}

// Lê um inteiro sem sinal de um campo JSON simples (por exemplo `completion_tokens`).
// O parser do projeto é deliberadamente mínimo e auditável: não interpreta objetos
// aninhados, apenas o par `"campo": número`.
[[nodiscard]] std::size_t json_unsigned_field(std::string_view json,
                                              std::string_view field) {
    const std::string needle = "\"" + std::string(field) + "\"";
    std::size_t position = json.find(needle);
    if (position == std::string_view::npos) return 0U;
    position += needle.size();
    while (position < json.size() && (json[position] == ' ' || json[position] == '\t'
                                      || json[position] == '\r' || json[position] == '\n')) {
        ++position;
    }
    if (position >= json.size() || json[position] != ':') return 0U;
    ++position;
    while (position < json.size() && (json[position] == ' ' || json[position] == '\t'
                                      || json[position] == '\r' || json[position] == '\n')) {
        ++position;
    }
    std::size_t value = 0U;
    bool any_digit = false;
    while (position < json.size() && json[position] >= '0' && json[position] <= '9') {
        const std::size_t digit = static_cast<std::size_t>(json[position] - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) return 0U;
        value = value * 10U + digit;
        any_digit = true;
        ++position;
    }
    return any_digit ? value : 0U;
}

std::string resolve_executable_path(std::string_view executable) {
    if (executable.empty() || executable.size() > 4'096U) {
        throw Error("executável vazio ou grande demais");
    }
    std::string name(executable);
#ifndef _WIN32
    return name;
#else
    // Caminhos sem separador podem ser resolvidos pelo PATH do Windows:
    // nesse caso não acrescentamos nada, porque o diretório atual não é
    // consultado da mesma forma que o PATH.
    const bool looks_like_path = name.find('/') != std::string::npos
                                 || name.find('\\') != std::string::npos;
    if (!looks_like_path) {
        return name;
    }
    std::error_code error;
    if (std::filesystem::is_regular_file(name, error)) return name;
    if (name.size() >= 4U
        && name.compare(name.size() - 4U, 4U, ".exe") == 0) {
        return name;
    }
    const std::string with_extension = name + ".exe";
    if (std::filesystem::is_regular_file(with_extension, error)) return with_extension;
    // Barra invertida e ausência de `./` aumentam a chance de o CreateProcess
    // resolver o caminho relativo quando o arquivo existe em outro formato.
    std::string normalized = name;
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    if (std::filesystem::is_regular_file(normalized, error)) return normalized;
    return name;
#endif
}

NativeHttpClient::NativeHttpClient(std::string executable)
    : executable_(std::move(executable)) {
    if (executable_.empty()) {
        const char* configured = std::getenv("CORTEX_CURL_PATH");
        executable_ = configured != nullptr && *configured != '\0' ? configured : "curl";
    }
}

void NativeHttpClient::validate_url(std::string_view url) {
    if (url.empty() || url.size() > 4'096U || url.find_first_of("\r\n\t ") != std::string_view::npos) {
        throw Error("URL vazia, grande demais ou com caracteres proibidos");
    }
    if (url.starts_with("https://")) {
        if (url.size() <= std::string_view("https://").size()) {
            throw Error("URL HTTPS sem host");
        }
        return;
    }
    if (local_http_url(url)) return;
    throw Error("somente HTTPS é permitido; HTTP é aceito apenas em localhost");
}

NativeHttpResponse NativeHttpClient::get(std::string_view url,
                                         std::size_t timeout_seconds,
                                         std::size_t maximum_response_bytes) const {
    return perform("GET", url, {}, {}, timeout_seconds, maximum_response_bytes);
}

NativeHttpResponse NativeHttpClient::post_json(
    std::string_view url,
    std::span<const std::pair<std::string, std::string>> headers,
    std::string_view json,
    std::size_t timeout_seconds,
    std::size_t maximum_response_bytes) const {
    return perform("POST", url, headers, json, timeout_seconds, maximum_response_bytes);
}

NativeHttpResponse NativeHttpClient::perform(
    std::string_view method,
    std::string_view url,
    std::span<const std::pair<std::string, std::string>> headers,
    std::string_view body,
    std::size_t timeout_seconds,
    std::size_t maximum_response_bytes) const {
    validate_url(url);
    if (timeout_seconds == 0U || timeout_seconds > 300U || maximum_response_bytes == 0U
        || maximum_response_bytes > 16U * 1024U * 1024U || body.size() > 2U * 1024U * 1024U) {
        throw Error("limites inválidos no cliente HTTP nativo");
    }

    std::vector<std::string> arguments{
        executable_, "--silent", "--show-error", "--location",
        "--max-time", std::to_string(timeout_seconds),
        "--connect-timeout", std::to_string(std::min<std::size_t>(8U, timeout_seconds)),
        "--proto", "=https,http", "--proto-redir", "=https",
        "--request", std::string(method),
        "--write-out", std::string(status_marker) + "%{http_code}"
    };
    std::string process_input;
    if (method == "POST") {
        // O corpo e todos os headers entram por um pipe de configuração. Isso evita
        // expor tanto a chave Bearer quanto a conversa em argv ou em arquivo temporário.
        std::ostringstream config;
        config << "header = " << curl_config_quote("Content-Type: application/json") << '\n';
        for (const auto& [name, value] : headers) {
            if (name.empty() || name.find_first_of("\r\n:") != std::string::npos
                || value.find_first_of("\r\n") != std::string::npos) {
                throw Error("cabeçalho de saída inválido");
            }
            config << "header = " << curl_config_quote(name + ": " + value) << '\n';
        }
        config << "data-binary = " << curl_config_quote(body) << '\n';
        process_input = config.str();
        arguments.emplace_back("--config");
        arguments.emplace_back("-");
    } else if (!headers.empty()) {
        throw Error("headers customizados são aceitos apenas em POST");
    }
    arguments.emplace_back("--url");
    arguments.emplace_back(url);

    ProcessResult process = run_process(
        arguments, process_input, timeout_seconds, maximum_response_bytes);
    const std::size_t marker = process.output.rfind(status_marker);
    if (marker == std::string::npos) {
        const std::string detail = process.output.empty()
                                   ? "curl indisponível ou terminou com código "
                                         + std::to_string(process.exit_code)
                                   : abbreviated(process.output);
        throw Error("cliente HTTP nativo falhou: " + detail);
    }
    const std::string status_text = process.output.substr(marker + status_marker.size());
    char* end = nullptr;
    const long status = std::strtol(status_text.c_str(), &end, 10);
    if (end == status_text.c_str() || (status != 0L && (status < 100L || status > 599L))) {
        throw Error("cliente HTTP não recebeu status válido");
    }
    process.output.resize(marker);
    if (process.output.size() > maximum_response_bytes) {
        throw Error("resposta HTTP excedeu o limite configurado");
    }
    if (status == 0L || process.exit_code != 0) {
        const std::string detail = process.output.empty()
                                   ? "curl terminou com código " + std::to_string(process.exit_code)
                                   : abbreviated(process.output);
        throw Error("cliente HTTP falhou: " + detail);
    }
    return NativeHttpResponse{status, std::move(process.output)};
}

LanguageApiClient::LanguageApiClient(LanguageApiConfig config, NativeHttpClient http)
    : config_(std::move(config)), http_(std::move(http)) {
    if (!config_.api_key_environment.empty()) {
        const char* secret = std::getenv(config_.api_key_environment.c_str());
        if (secret != nullptr) api_key_ = secret;
    }
    if (!config_.reasoning_effort.empty() && config_.reasoning_effort != "low"
        && config_.reasoning_effort != "medium" && config_.reasoning_effort != "high") {
        throw Error("reasoning_effort deve ser low, medium ou high");
    }
    if (config_.extended_sampling
        && (!(config_.top_p > 0.0F && config_.top_p <= 1.0F)
            || !(config_.presence_penalty >= 0.0F && config_.presence_penalty <= 2.0F))) {
        throw Error("amostragem estendida fora dos limites permitidos");
    }
    if (!config_.endpoint.empty()) NativeHttpClient::validate_url(config_.endpoint);
}

bool LanguageApiClient::configured() const noexcept {
    return !config_.endpoint.empty() && !config_.model.empty()
           && (!api_key_.empty() || local_http_url(config_.endpoint));
}

LanguageCompletion LanguageApiClient::complete(std::string_view system_prompt,
                                                std::string_view user_prompt,
                                                float temperature,
                                                std::size_t maximum_tokens,
                                                std::size_t top_k,
                                                LanguageReasoningMode reasoning_mode) const {
    if (!configured()) {
        throw Error("API de linguagem não configurada; defina endpoint, modelo e chave");
    }
    if (!(temperature >= 0.0F && temperature <= 2.0F) || maximum_tokens == 0U
        || maximum_tokens > 8'192U || top_k == 0U || top_k > 100'000U
        || (reasoning_mode != LanguageReasoningMode::disabled
            && reasoning_mode != LanguageReasoningMode::low_effort
            && reasoning_mode != LanguageReasoningMode::full)) {
        throw Error("parâmetros inválidos para API de linguagem");
    }
    const float effective_temperature = config_.requires_reasoning_sampling ? 1.0F : temperature;
    const float effective_top_p = config_.requires_reasoning_sampling ? 0.95F : config_.top_p;

    std::ostringstream request;
    request << "{\"model\":" << json_quote(config_.model)
            << ",\"messages\":[{\"role\":\"system\",\"content\":"
            << json_quote(system_prompt)
            << "},{\"role\":\"user\",\"content\":" << json_quote(user_prompt)
            << "}],\"temperature\":" << std::setprecision(3) << effective_temperature
            << ",\"max_tokens\":" << maximum_tokens;
    if (!config_.reasoning_effort.empty()) {
        request << ",\"reasoning_effort\":" << json_quote(config_.reasoning_effort);
    }
    if (config_.extended_sampling) {
        request << ",\"top_k\":" << top_k
                << ",\"top_p\":" << effective_top_p
                << ",\"presence_penalty\":" << config_.presence_penalty;
    }
    if (config_.supports_reasoning_control) {
        const bool enabled = reasoning_mode != LanguageReasoningMode::disabled;
        request << ",\"chat_template_kwargs\":{\"enable_thinking\":"
                << (enabled ? "true" : "false")
                << ",\"low_effort\":"
                << (reasoning_mode == LanguageReasoningMode::low_effort ? "true" : "false")
                << '}';
    }
    request << '}';

    std::vector<std::pair<std::string, std::string>> headers;
    if (!api_key_.empty()) headers.emplace_back("Authorization", "Bearer " + api_key_);
    const NativeHttpResponse response = http_.post_json(
        config_.endpoint, headers, request.str(), config_.timeout_seconds);
    if (response.status < 200L || response.status >= 300L) {
        const std::string message = json_string_field(response.body, "message")
                                    .value_or(abbreviated(response.body));
        throw Error("API de linguagem retornou HTTP " + std::to_string(response.status)
                    + ": " + message);
    }

    std::optional<std::string> content = json_string_field(response.body, "content");
    if (!content || content->empty()) content = json_string_field(response.body, "output_text");
    if (!content || content->empty()) {
        // Um modelo com raciocínio pode consumir todo o orçamento dentro do
        // bloco de pensamento. Distinguir esse caso evita tratar um limite de
        // tokens como resposta malformada do servidor.
        const std::optional<std::string> reasoning =
            json_string_field(response.body, "reasoning_content");
        if (reasoning && !reasoning->empty()) {
            throw Error("modelo encerrou o orçamento dentro do raciocínio, sem resposta final");
        }
        throw Error("API de linguagem respondeu sem campo content");
    }
    LanguageCompletion completion;
    completion.text = without_private_reasoning(std::move(*content));
    completion.model = config_.display_model.empty() ? config_.model : config_.display_model;
    completion.reasoning = without_private_reasoning(
        json_string_field(response.body, "reasoning_content").value_or(std::string{}));
    completion.prompt_tokens = json_unsigned_field(response.body, "prompt_tokens");
    completion.generated_tokens = json_unsigned_field(response.body, "completion_tokens");
    return completion;
}

InternetSearch::InternetSearch(NativeHttpClient http) : http_(std::move(http)) {}

std::vector<InternetResult> InternetSearch::search_multiple(
    std::string_view query, std::size_t maximum_results) const {
    if (query.empty() || query.size() > 1'024U) {
        throw Error("consulta de internet vazia ou grande demais");
    }
    if (maximum_results == 0U || maximum_results > 8U) {
        throw Error("quantidade de resultados de internet fora do limite");
    }

    std::vector<InternetResult> results;
    results.reserve(std::min<std::size_t>(maximum_results, 2U));
    bool source_reached = false;
    std::string failures;
    try {
        const std::string url = "https://api.duckduckgo.com/?q=" + url_encode(query)
            + "&format=json&no_html=1&no_redirect=1&skip_disambig=1&kl=br-pt";
        const NativeHttpResponse response = http_.get(url, 12U, 1U * 1024U * 1024U);
        if (response.status < 200L || response.status >= 300L) {
            throw Error("DuckDuckGo retornou HTTP " + std::to_string(response.status));
        }
        source_reached = true;
        InternetResult result;
        result.source_name = "DuckDuckGo";
        result.heading = json_string_field(response.body, "Heading").value_or("");
        result.summary = json_string_field(response.body, "AbstractText").value_or("");
        result.source_url = json_string_field(response.body, "AbstractURL").value_or("");
        if (result.summary.empty()) {
            result.summary = json_string_field(response.body, "Answer").value_or("");
        }
        if (result.summary.empty()) {
            result.summary = json_string_field(response.body, "Text").value_or("");
            if (result.source_url.empty()) {
                result.source_url = json_string_field(response.body, "FirstURL").value_or("");
            }
        }
        if (!result.empty()) {
            limit_internet_result(result);
            results.push_back(std::move(result));
        }
    } catch (const std::exception& exception) {
        failures = exception.what();
    }

    if (results.size() < maximum_results) {
        try {
            const std::string url =
                "https://pt.wikipedia.org/w/api.php?action=query&generator=search&gsrnamespace=0"
                "&gsrlimit=1&prop=extracts%7Cinfo&exintro=1&explaintext=1&inprop=url"
                "&format=json&formatversion=2&redirects=1&gsrsearch=" + url_encode(query);
            const NativeHttpResponse response = http_.get(url, 12U, 1U * 1024U * 1024U);
            if (response.status < 200L || response.status >= 300L) {
                throw Error("Wikipedia retornou HTTP " + std::to_string(response.status));
            }
            source_reached = true;
            InternetResult result;
            result.source_name = "Wikipedia (pt)";
            result.heading = json_string_field(response.body, "title").value_or("");
            result.summary = json_string_field(response.body, "extract").value_or("");
            result.source_url = json_string_field(response.body, "fullurl").value_or("");
            if (!result.empty()) {
                limit_internet_result(result);
                results.push_back(std::move(result));
            }
        } catch (const std::exception& exception) {
            if (!failures.empty()) failures += "; ";
            failures += exception.what();
        }
    }

    if (results.empty() && !failures.empty()) {
        throw Error((source_reached
            ? std::string("pesquisa incompleta; uma fonte não respondeu: ")
            : std::string("fontes de pesquisa indisponíveis: ")) + failures);
    }
    return results;
}

InternetResult InternetSearch::search(std::string_view query) const {
    std::vector<InternetResult> results = search_multiple(query, 1U);
    if (results.empty()) return {};
    return std::move(results.front());
}

} // namespace cortex
