#include "cortex/web_server.hpp"

#include "cortex/core.hpp"
#include "cortex/self_editor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace cortex {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;
#endif

constexpr std::size_t maximum_header_bytes = 16U * 1024U;
constexpr std::size_t maximum_message_bytes = 16U * 1024U;
constexpr std::size_t maximum_queued_connections = 128U;

class SocketEnvironment final {
public:
    SocketEnvironment() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw Error("WSAStartup falhou ao iniciar o servidor HTTP");
        }
#endif
    }

    ~SocketEnvironment() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

void close_socket(NativeSocket socket) noexcept {
    if (socket == invalid_socket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

[[nodiscard]] int last_socket_error() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

[[nodiscard]] bool interrupted_socket_error(int error) noexcept {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

class SocketGuard final {
public:
    explicit SocketGuard(NativeSocket socket = invalid_socket) : socket_(socket) {}
    ~SocketGuard() { close_socket(socket_); }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    [[nodiscard]] NativeSocket get() const noexcept { return socket_; }
    [[nodiscard]] NativeSocket release() noexcept {
        const NativeSocket result = socket_;
        socket_ = invalid_socket;
        return result;
    }

private:
    NativeSocket socket_;
};

class HttpFailure final : public std::runtime_error {
public:
    HttpFailure(int status, std::string message)
        : std::runtime_error(std::move(message)), status_(status) {}

    [[nodiscard]] int status() const noexcept { return status_; }

private:
    int status_;
};

struct HttpRequest final {
    std::string method;
    std::string target;
    std::string content_type;
    std::string body;
};

struct HttpResponse final {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

[[nodiscard]] std::string ascii_lower(std::string_view input) {
    std::string result(input);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

[[nodiscard]] std::string_view trim_ascii(std::string_view input) {
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t')) {
        input.remove_prefix(1U);
    }
    while (!input.empty() && (input.back() == ' ' || input.back() == '\t')) {
        input.remove_suffix(1U);
    }
    return input;
}

[[nodiscard]] std::size_t parse_content_length(std::string_view input) {
    if (input.empty()) {
        throw HttpFailure(400, "Content-Length vazio");
    }
    std::size_t result = 0U;
    for (const char character : input) {
        if (character < '0' || character > '9') {
            throw HttpFailure(400, "Content-Length inválido");
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            throw HttpFailure(413, "Content-Length excede o limite numérico");
        }
        result = result * 10U + digit;
    }
    return result;
}

[[nodiscard]] int receive_some(NativeSocket socket, char* buffer, std::size_t capacity) {
#ifdef _WIN32
    return recv(socket, buffer, static_cast<int>(capacity), 0);
#else
    return static_cast<int>(recv(socket, buffer, capacity, 0));
#endif
}

void send_all(NativeSocket socket, std::string_view data) {
    std::size_t sent = 0U;
    while (sent < data.size()) {
        const std::size_t remaining = data.size() - sent;
#ifdef _WIN32
        const int amount = send(socket, data.data() + sent,
                                static_cast<int>(std::min<std::size_t>(remaining,
                                                                      static_cast<std::size_t>(INT_MAX))),
                                0);
#else
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const int amount = static_cast<int>(send(socket, data.data() + sent, remaining, flags));
#endif
        if (amount <= 0) {
            throw Error("cliente desconectou durante envio HTTP");
        }
        sent += static_cast<std::size_t>(amount);
    }
}

void configure_client_socket(NativeSocket socket) {
#ifdef _WIN32
    const DWORD timeout = 15'000U;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    const timeval timeout{15, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

[[nodiscard]] HttpRequest read_http_request(NativeSocket socket,
                                            std::size_t maximum_body_bytes) {
    std::string data;
    data.reserve(4'096U);
    std::array<char, 4'096U> buffer{};
    std::size_t header_end = std::string::npos;

    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        if (data.size() >= maximum_header_bytes) {
            throw HttpFailure(431, "cabeçalhos HTTP excedem 16 KiB");
        }
        const int received = receive_some(socket, buffer.data(), buffer.size());
        if (received <= 0) {
            throw HttpFailure(400, "conexão encerrada antes dos cabeçalhos HTTP");
        }
        data.append(buffer.data(), static_cast<std::size_t>(received));
    }

    if (header_end > maximum_header_bytes) {
        throw HttpFailure(431, "cabeçalhos HTTP excedem 16 KiB");
    }
    const std::string_view header_block(data.data(), header_end);
    const std::size_t first_line_end = header_block.find("\r\n");
    const std::string_view request_line = first_line_end == std::string_view::npos
                                          ? header_block
                                          : header_block.substr(0U, first_line_end);
    const std::size_t first_space = request_line.find(' ');
    const std::size_t second_space = first_space == std::string_view::npos
                                     ? std::string_view::npos
                                     : request_line.find(' ', first_space + 1U);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
        throw HttpFailure(400, "linha de requisição HTTP inválida");
    }

    HttpRequest request;
    request.method = std::string(request_line.substr(0U, first_space));
    request.target = std::string(request_line.substr(first_space + 1U,
                                                     second_space - first_space - 1U));
    const std::string_view version = request_line.substr(second_space + 1U);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        throw HttpFailure(505, "versão HTTP não suportada");
    }
    if (request.target.empty() || request.target.front() != '/' || request.target.size() > 2'048U) {
        throw HttpFailure(400, "target HTTP inválido");
    }

    std::map<std::string, std::string, std::less<>> headers;
    std::size_t cursor = first_line_end == std::string_view::npos
                         ? header_block.size()
                         : first_line_end + 2U;
    while (cursor < header_block.size()) {
        const std::size_t end = header_block.find("\r\n", cursor);
        const std::string_view line = header_block.substr(
            cursor, (end == std::string_view::npos ? header_block.size() : end) - cursor);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            throw HttpFailure(400, "cabeçalho HTTP sem separador");
        }
        const std::string name = ascii_lower(trim_ascii(line.substr(0U, colon)));
        const std::string value(trim_ascii(line.substr(colon + 1U)));
        if (name.empty() || headers.contains(name)) {
            throw HttpFailure(400, "cabeçalho HTTP vazio ou duplicado");
        }
        headers.emplace(name, value);
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 2U;
    }

    if (headers.contains("transfer-encoding")) {
        throw HttpFailure(400, "Transfer-Encoding não é aceito; use Content-Length");
    }
    const auto type = headers.find("content-type");
    if (type != headers.end()) {
        request.content_type = ascii_lower(type->second);
    }

    std::size_t content_length = 0U;
    const auto length = headers.find("content-length");
    if (length != headers.end()) {
        content_length = parse_content_length(length->second);
    }
    if (content_length > maximum_body_bytes) {
        throw HttpFailure(413, "corpo HTTP excede o limite configurado");
    }
    if (request.method == "POST" && length == headers.end()) {
        throw HttpFailure(411, "requisição POST exige Content-Length");
    }

    const std::size_t body_begin = header_end + 4U;
    while (data.size() - body_begin < content_length) {
        const int received = receive_some(socket, buffer.data(), buffer.size());
        if (received <= 0) {
            throw HttpFailure(400, "conexão encerrada antes do corpo HTTP");
        }
        data.append(buffer.data(), static_cast<std::size_t>(received));
        if (data.size() - body_begin > maximum_body_bytes) {
            throw HttpFailure(413, "corpo HTTP excede o limite configurado");
        }
    }
    request.body.assign(data.data() + static_cast<std::ptrdiff_t>(body_begin), content_length);

    const std::size_t query = request.target.find('?');
    if (query != std::string::npos) {
        request.target.resize(query);
    }
    return request;
}

[[nodiscard]] std::string status_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Error";
    }
}

void write_http_response(NativeSocket socket, const HttpResponse& response) {
    std::ostringstream headers;
    headers << "HTTP/1.1 " << response.status << ' ' << status_reason(response.status) << "\r\n"
            << "Content-Type: " << response.content_type << "\r\n"
            << "Content-Length: " << response.body.size() << "\r\n"
            << "Connection: close\r\n"
            << "Cache-Control: no-store\r\n"
            << "X-Content-Type-Options: nosniff\r\n"
            << "Referrer-Policy: no-referrer\r\n";
    for (const auto& [name, value] : response.headers) {
        headers << name << ": " << value << "\r\n";
    }
    headers << "\r\n";
    send_all(socket, headers.str());
    if (!response.body.empty()) {
        send_all(socket, response.body);
    }
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

class JsonParser final {
public:
    enum class Type { string, number, boolean, null_value };

    struct Value final {
        Type type = Type::null_value;
        std::string text;
        double number = 0.0;
        bool boolean = false;
    };

    explicit JsonParser(std::string_view input) : input_(input) {}

    [[nodiscard]] std::unordered_map<std::string, Value> parse_object() {
        skip_whitespace();
        expect('{');
        skip_whitespace();
        std::unordered_map<std::string, Value> result;
        if (consume('}')) {
            finish();
            return result;
        }

        while (true) {
            if (result.size() >= 32U) {
                fail("objeto JSON contém campos demais");
            }
            const std::string key = parse_string();
            if (key.size() > 64U) {
                fail("nome de campo JSON longo demais");
            }
            skip_whitespace();
            expect(':');
            skip_whitespace();
            Value value = parse_value();
            if (!result.emplace(key, std::move(value)).second) {
                fail("campo JSON duplicado");
            }
            skip_whitespace();
            if (consume('}')) {
                finish();
                return result;
            }
            expect(',');
            skip_whitespace();
        }
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw HttpFailure(400, std::string(message));
    }

    void skip_whitespace() {
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
                break;
            }
            ++position_;
        }
    }

    void expect(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) {
            fail("JSON malformado");
        }
        ++position_;
    }

    [[nodiscard]] bool consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::uint32_t hex4() {
        if (position_ + 4U > input_.size()) {
            fail("escape Unicode JSON truncado");
        }
        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const char character = input_[position_++];
            value <<= 4U;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                fail("escape Unicode JSON inválido");
            }
        }
        return value;
    }

    [[nodiscard]] std::string parse_string() {
        expect('"');
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') {
                return result;
            }
            if (character < 0x20U) {
                fail("controle não escapado em string JSON");
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) {
                fail("escape JSON truncado");
            }
            const char escaped = input_[position_++];
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
                std::uint32_t codepoint = hex4();
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (position_ + 2U > input_.size()
                        || input_[position_] != '\\' || input_[position_ + 1U] != 'u') {
                        fail("surrogate Unicode alto sem par");
                    }
                    position_ += 2U;
                    const std::uint32_t low = hex4();
                    if (low < 0xDC00U || low > 0xDFFFU) {
                        fail("par surrogate Unicode inválido");
                    }
                    codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U)
                                + (low - 0xDC00U);
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    fail("surrogate Unicode baixo sem par");
                }
                append_utf8(result, codepoint);
                break;
            }
            default: fail("escape JSON desconhecido");
            }
        }
        fail("string JSON sem fechamento");
    }

    [[nodiscard]] Value parse_number() {
        const std::size_t begin = position_;
        if (consume('-')) {}
        if (consume('0')) {
            // Zero inicial não pode ser seguido diretamente por outro dígito.
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                fail("número JSON com zero inicial");
            }
        } else {
            if (position_ >= input_.size() || input_[position_] < '1' || input_[position_] > '9') {
                fail("número JSON inválido");
            }
            while (position_ < input_.size()
                   && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            const std::size_t fraction_begin = position_;
            while (position_ < input_.size()
                   && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (position_ == fraction_begin) {
                fail("fração JSON vazia");
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_begin = position_;
            while (position_ < input_.size()
                   && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (position_ == exponent_begin) {
                fail("expoente JSON vazio");
            }
        }

        const std::string encoded(input_.substr(begin, position_ - begin));
        char* end = nullptr;
        errno = 0;
        const double number = std::strtod(encoded.c_str(), &end);
        if (errno == ERANGE || end != encoded.c_str() + encoded.size() || !std::isfinite(number)) {
            fail("número JSON fora do intervalo");
        }
        Value value;
        value.type = Type::number;
        value.number = number;
        return value;
    }

    [[nodiscard]] bool consume_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) == literal) {
            position_ += literal.size();
            return true;
        }
        return false;
    }

    [[nodiscard]] Value parse_value() {
        if (position_ >= input_.size()) {
            fail("valor JSON ausente");
        }
        if (input_[position_] == '"') {
            Value value;
            value.type = Type::string;
            value.text = parse_string();
            return value;
        }
        if (input_[position_] == '-' || (input_[position_] >= '0' && input_[position_] <= '9')) {
            return parse_number();
        }
        if (consume_literal("true")) {
            Value value;
            value.type = Type::boolean;
            value.boolean = true;
            return value;
        }
        if (consume_literal("false")) {
            Value value;
            value.type = Type::boolean;
            value.boolean = false;
            return value;
        }
        if (consume_literal("null")) {
            return Value{};
        }
        fail("tipo JSON não suportado");
    }

    void finish() {
        skip_whitespace();
        if (position_ != input_.size()) {
            fail("dados extras depois do objeto JSON");
        }
    }

    std::string_view input_;
    std::size_t position_ = 0U;
};

using JsonObject = std::unordered_map<std::string, JsonParser::Value>;

[[nodiscard]] const JsonParser::Value* json_field(const JsonObject& object,
                                                   std::string_view key) {
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] std::string required_json_string(const JsonObject& object,
                                                std::string_view key) {
    const JsonParser::Value* value = json_field(object, key);
    if (value == nullptr || value->type != JsonParser::Type::string) {
        throw HttpFailure(422, "campo JSON obrigatório ausente ou não textual: "
                               + std::string(key));
    }
    return value->text;
}

[[nodiscard]] std::string optional_json_string(const JsonObject& object,
                                                std::string_view key,
                                                std::string fallback = {}) {
    const JsonParser::Value* value = json_field(object, key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->type != JsonParser::Type::string) {
        throw HttpFailure(422, "campo JSON deve ser textual: " + std::string(key));
    }
    return value->text;
}

[[nodiscard]] double optional_json_number(const JsonObject& object,
                                           std::string_view key,
                                           double fallback) {
    const JsonParser::Value* value = json_field(object, key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->type != JsonParser::Type::number) {
        throw HttpFailure(422, "campo JSON deve ser numérico: " + std::string(key));
    }
    return value->number;
}

[[nodiscard]] bool optional_json_bool(const JsonObject& object,
                                      std::string_view key,
                                      bool fallback) {
    const JsonParser::Value* value = json_field(object, key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->type != JsonParser::Type::boolean) {
        throw HttpFailure(422, "campo JSON deve ser booleano: " + std::string(key));
    }
    return value->boolean;
}

[[nodiscard]] std::size_t bounded_integer(double value,
                                          std::size_t minimum,
                                          std::size_t maximum,
                                          std::string_view field) {
    if (!std::isfinite(value) || value != std::floor(value)
        || value < static_cast<double>(minimum) || value > static_cast<double>(maximum)) {
        throw HttpFailure(422, "campo fora do intervalo permitido: " + std::string(field));
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] bool continuation(unsigned char byte) noexcept {
    return byte >= 0x80U && byte <= 0xBFU;
}

[[nodiscard]] std::string sanitize_utf8(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    std::size_t index = 0U;
    while (index < input.size()) {
        const unsigned char first = static_cast<unsigned char>(input[index]);
        std::size_t length = 0U;
        bool valid = false;
        if (first <= 0x7FU) {
            length = 1U;
            valid = true;
        } else if (first >= 0xC2U && first <= 0xDFU && index + 1U < input.size()) {
            length = 2U;
            valid = continuation(static_cast<unsigned char>(input[index + 1U]));
        } else if (first >= 0xE0U && first <= 0xEFU && index + 2U < input.size()) {
            const unsigned char second = static_cast<unsigned char>(input[index + 1U]);
            const unsigned char third = static_cast<unsigned char>(input[index + 2U]);
            const bool scalar_range = (first != 0xE0U || second >= 0xA0U)
                                      && (first != 0xEDU || second <= 0x9FU);
            length = 3U;
            valid = scalar_range && continuation(second) && continuation(third);
        } else if (first >= 0xF0U && first <= 0xF4U && index + 3U < input.size()) {
            const unsigned char second = static_cast<unsigned char>(input[index + 1U]);
            const bool scalar_range = (first != 0xF0U || second >= 0x90U)
                                      && (first != 0xF4U || second <= 0x8FU);
            length = 4U;
            valid = scalar_range && continuation(second)
                    && continuation(static_cast<unsigned char>(input[index + 2U]))
                    && continuation(static_cast<unsigned char>(input[index + 3U]));
        }

        if (valid) {
            result.append(input.substr(index, length));
            index += length;
        } else {
            result += "\xEF\xBF\xBD"; // U+FFFD mantém respostas JSON sempre válidas.
            ++index;
        }
    }
    return result;
}

[[nodiscard]] std::string json_escape(std::string_view input) {
    const std::string valid = sanitize_utf8(input);
    std::ostringstream escaped;
    escaped << '"';
    for (const unsigned char character : valid) {
        switch (character) {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20U) {
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(character) << std::dec;
            } else {
                escaped << static_cast<char>(character);
            }
        }
    }
    escaped << '"';
    return escaped.str();
}

[[nodiscard]] HttpResponse json_error_response(int status, std::string_view message) {
    return HttpResponse{status,
                        "application/json; charset=utf-8",
                        "{\"ok\":false,\"error\":" + json_escape(message) + "}",
                        {}};
}

class WebRuntime final {
public:
    WebRuntime(const BpeTokenizer& tokenizer,
               const NeuralLanguageModel& model,
               LongTermMemory& memory,
               WebServerOptions options)
        : tokenizer_(tokenizer),
          model_(model),
          memory_(memory),
          options_(std::move(options)),
          language_api_(options_.language_api),
          internet_(),
          self_editor_(options_.source_root, language_api_, options_.self_edit_enabled) {}

    [[nodiscard]] bool language_api_configured() const noexcept {
        return language_api_.configured();
    }

    [[nodiscard]] HttpResponse route(const HttpRequest& request) {
        if (request.method == "GET" && request.target == "/") {
            HttpResponse response;
            response.content_type = "text/html; charset=utf-8";
            response.body = std::string(web_chat_html());
            response.headers.emplace_back(
                "Content-Security-Policy",
                "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                "connect-src 'self'; img-src data:; base-uri 'none'; form-action 'self'");
            return response;
        }
        if (request.method == "GET" && request.target == "/health") {
            return HttpResponse{200, "text/plain; charset=utf-8", "ok\n", {}};
        }
        if (request.method == "GET" && request.target == "/api/status") {
            return status_response();
        }
        if (request.method == "GET" && request.target == "/favicon.ico") {
            return HttpResponse{204, "image/x-icon", {}, {}};
        }
        if (request.method == "POST" && request.target == "/api/chat") {
            require_json_content_type(request);
            return chat_response(request.body);
        }
        if (request.method == "POST" && request.target == "/api/remember") {
            require_json_content_type(request);
            return remember_response(request.body);
        }
        if (request.method == "POST" && request.target == "/api/code/propose") {
            require_json_content_type(request);
            return code_proposal_response(request.body);
        }
        if (request.method == "POST" && request.target == "/api/code/apply") {
            require_json_content_type(request);
            return code_apply_response(request.body);
        }
        if (request.target == "/api/chat" || request.target == "/api/remember"
            || request.target == "/api/status" || request.target == "/api/code/propose"
            || request.target == "/api/code/apply") {
            return json_error_response(405, "método HTTP não permitido para este endpoint");
        }
        return json_error_response(404, "endpoint não encontrado");
    }

private:
    static void require_json_content_type(const HttpRequest& request) {
        if (request.content_type.find("application/json") == std::string::npos) {
            throw HttpFailure(415, "Content-Type deve ser application/json");
        }
    }

    [[nodiscard]] HttpResponse status_response() const {
        const bool api_configured = language_api_.configured();
        const bool api_requested = !language_api_.config().endpoint.empty()
                                   && !language_api_.config().model.empty();
        const std::string_view language_provider = api_requested
            ? std::string_view(language_api_.config().provider) : std::string_view("local-gru");
        const std::string_view language_state = api_configured ? "ready"
                                                : api_requested ? "missing-key"
                                                                : "disabled";
        const std::string_view display_model = !language_api_.config().display_model.empty()
            ? std::string_view(language_api_.config().display_model)
            : std::string_view(language_api_.config().model);
        std::ostringstream body;
        body << "{\"ok\":true,\"vocabulary\":" << model_.config().vocabulary_size
             << ",\"hidden\":" << model_.config().hidden_size
             << ",\"layers\":" << model_.config().layer_count
             << ",\"parameters\":" << model_.parameter_count()
             << ",\"memories\":" << memory_.size()
             << ",\"language_api\":" << (api_configured ? "true" : "false")
             << ",\"language_api_state\":" << json_escape(language_state)
             << ",\"language_provider\":" << json_escape(language_provider)
             << ",\"language_model\":"
             << json_escape(api_requested ? display_model : std::string_view("local"))
             << ",\"language_parameters\":" << language_api_.config().parameter_count
             << ",\"language_context\":" << language_api_.config().context_tokens
             << ",\"internet\":" << (options_.internet_enabled ? "true" : "false")
             << ",\"self_edit\":"
             << (self_editor_.enabled() && language_api_.configured() ? "true" : "false")
             << '}';
        return HttpResponse{200, "application/json; charset=utf-8", body.str(), {}};
    }

    [[nodiscard]] HttpResponse chat_response(std::string_view encoded) {
        const JsonObject object = JsonParser(encoded).parse_object();
        std::string message = required_json_string(object, "message");
        const std::string_view trimmed = trim_ascii(message);
        message.assign(trimmed.begin(), trimmed.end());
        message = sanitize_utf8(message);
        if (message.empty() || message.size() > maximum_message_bytes) {
            throw HttpFailure(422, "message deve conter entre 1 e 16000 bytes");
        }
        std::string conversation = sanitize_utf8(
            optional_json_string(object, "conversation"));
        if (conversation.size() > maximum_message_bytes) {
            throw HttpFailure(422, "conversation excede 16000 bytes");
        }

        AgentOptions agent_options = options_.agent;
        const double temperature = optional_json_number(
            object, "temperature", static_cast<double>(agent_options.generation.temperature));
        if (temperature < 0.1 || temperature > 2.0) {
            throw HttpFailure(422, "temperature deve estar entre 0.1 e 2.0");
        }
        agent_options.generation.temperature = static_cast<float>(temperature);
        agent_options.generation.top_k = bounded_integer(
            optional_json_number(object, "top_k",
                                 static_cast<double>(agent_options.generation.top_k)),
            1U, model_.config().vocabulary_size, "top_k");
        // APIs de fronteira precisam de uma janela maior; o fallback GRU é limitado
        // separadamente abaixo para continuar responsivo em uma máquina local.
        agent_options.generation.maximum_tokens = bounded_integer(
            optional_json_number(object, "max_tokens",
                                 static_cast<double>(agent_options.generation.maximum_tokens)),
            1U, 4'096U, "max_tokens");
        agent_options.memory_hits = bounded_integer(
            optional_json_number(object, "memory_hits",
                                 static_cast<double>(agent_options.memory_hits)),
            0U, 16U, "memory_hits");
        agent_options.remember_interaction = optional_json_bool(
            object, "remember", agent_options.remember_interaction);
        agent_options.generation.seed += request_sequence_.fetch_add(1U,
                                                                     std::memory_order_relaxed);

        const bool request_language_api = optional_json_bool(object, "language_api", true);
        const bool request_internet = optional_json_bool(object, "internet", false)
                                      && options_.internet_enabled;
        CognitiveAgent agent(tokenizer_, model_, memory_);
        const auto started = std::chrono::steady_clock::now();
        std::vector<MemoryHit> recalled = agent.recall(
            message, agent_options.memory_hits, agent_options.minimum_memory_score);
        InternetResult internet_result;
        std::string warning;
        if (request_language_api && !language_api_.configured()
            && !language_api_.config().endpoint.empty()
            && !language_api_.config().model.empty()) {
            const bool gemini_requested = language_api_.config().provider == "gemini";
            warning = gemini_requested
                      ? "Gemini sem chave; configure GEMINI_API_KEY no ambiente e reinicie. "
                        "Usando modelo local."
                      : "API linguística sem chave; usando modelo local.";
        }
        if (request_internet) {
            try {
                internet_result = internet_.search(message);
            } catch (const std::exception& exception) {
                if (!warning.empty()) warning += "; ";
                warning += "internet indisponível: " + std::string(exception.what());
            }
        }

        std::string answer;
        std::string provider = "local-gru";
        std::string provider_model = "GRU " + std::to_string(model_.config().layer_count)
                                     + "x" + std::to_string(model_.config().hidden_size);
        std::size_t generated_tokens = 0U;
        std::uint64_t stored_memory_id = 0U;

        if (request_language_api && language_api_.configured()) {
            try {
                std::ostringstream enriched;
                if (!conversation.empty()) {
                    enriched << "CONVERSA RECENTE:\n" << conversation << "\n\n";
                }
                if (!recalled.empty()) {
                    enriched << "MEMÓRIAS RELEVANTES:\n";
                    for (const MemoryHit& hit : recalled) {
                        enriched << "- " << hit.text.substr(0U, 1'024U) << '\n';
                    }
                    enriched << '\n';
                }
                if (!internet_result.empty()) {
                    enriched << "INFORMAÇÃO RECENTE DA INTERNET:\n"
                             << internet_result.heading << " — " << internet_result.summary;
                    if (!internet_result.source_url.empty()) {
                        enriched << "\nFonte: " << internet_result.source_url;
                    }
                    enriched << "\n\n";
                }
                const std::string current_question = "PERGUNTA ATUAL:\n" + message;
                enriched << current_question;
                std::string system =
                    "Você é Cortex, um assistente preciso e útil. Responda no idioma do usuário, "
                    "com clareza e diretamente à pergunta atual. Raciocine antes de responder, mas "
                    "mostre somente a explicação necessária. Não invente fatos. Use conversa, "
                    "memórias e informação da internet somente quando forem relevantes; cite a "
                    "fonte fornecida para fatos recentes. Se não souber, admita isso brevemente.";
                if (language_api_.config().provider == "local-open-model") {
                    // Qwen3 economiza a janela no diálogo diário; /think do usuário ainda prevalece.
                    system += "\n/no_think";
                }

                std::string api_prompt = enriched.str();
                std::size_t api_maximum_tokens = agent_options.generation.maximum_tokens;
                const std::size_t context_tokens = language_api_.config().context_tokens;
                if (context_tokens != 0U) {
                    // Reserva metade da janela para saída e uma margem para system/chat template.
                    api_maximum_tokens = std::min(
                        api_maximum_tokens, std::max<std::size_t>(1U, context_tokens / 2U));
                    constexpr std::size_t template_reserve = 256U;
                    const std::size_t input_tokens = context_tokens
                        > api_maximum_tokens + template_reserve
                        ? context_tokens - api_maximum_tokens - template_reserve
                        : std::max<std::size_t>(128U, context_tokens / 4U);
                    const std::size_t byte_budget = std::max<std::size_t>(1'024U, input_tokens * 3U);
                    if (api_prompt.size() > byte_budget) {
                        constexpr std::string_view notice = "[Contexto truncado]\n";
                        if (current_question.size() + notice.size() <= byte_budget) {
                            // Descarta primeiro o contexto antigo e preserva a pergunta inteira.
                            const std::size_t context_tail = byte_budget - notice.size()
                                                             - current_question.size();
                            const std::size_t context_size = api_prompt.size()
                                                             - current_question.size();
                            api_prompt = std::string(notice)
                                         + sanitize_utf8(std::string_view(api_prompt).substr(
                                               context_size - std::min(context_size, context_tail),
                                               std::min(context_size, context_tail)))
                                         + current_question;
                        } else {
                            // Uma pergunta enorme conserva começo e fim em vez de falhar por contexto.
                            const std::size_t available = byte_budget - notice.size();
                            const std::size_t first = available / 2U;
                            const std::size_t last = available - first;
                            api_prompt = sanitize_utf8(std::string_view(current_question).substr(0U, first))
                                         + std::string(notice)
                                         + sanitize_utf8(std::string_view(current_question).substr(
                                               current_question.size() - last));
                        }
                    }
                }
                LanguageCompletion completion = language_api_.complete(
                    system, api_prompt, agent_options.generation.temperature,
                    api_maximum_tokens, agent_options.generation.top_k);
                answer = sanitize_utf8(completion.text);
                const auto response_whitespace = [](char character) {
                    return character == ' ' || character == '\t' || character == '\r'
                           || character == '\n';
                };
                while (!answer.empty() && response_whitespace(answer.front())) answer.erase(0U, 1U);
                while (!answer.empty() && response_whitespace(answer.back())) answer.pop_back();
                if (answer.empty()) throw Error("API de linguagem devolveu texto vazio");
                provider = language_api_.config().provider == "local-open-model"
                           ? "local-open-model" : "language-api";
                provider_model = completion.model;
            } catch (const std::exception& exception) {
                if (!warning.empty()) warning += "; ";
                warning += language_api_.config().provider == "local-open-model"
                           ? "modelo GGUF falhou; acionando fallback seguro: "
                           : "API de linguagem falhou; acionando fallback local: ";
                warning += exception.what();
            }
        }

        // Sem uma API linguística, um resultado web objetivo ainda é melhor que inventar.
        if (answer.empty() && request_internet && !internet_result.empty()) {
            answer = internet_result.summary;
            if (!internet_result.source_url.empty()) answer += "\n\nFonte: " + internet_result.source_url;
            provider = "internet";
            provider_model = "DuckDuckGo Instant Answer";
        }

        if (answer.empty()) {
            AgentOptions local_options = agent_options;
            local_options.generation.maximum_tokens = std::min<std::size_t>(
                local_options.generation.maximum_tokens, 512U);
            AgentReply local = agent.respond(message, local_options, conversation);
            answer = std::move(local.text);
            generated_tokens = local.generated_tokens.size();
            stored_memory_id = local.stored_memory_id;
            recalled = std::move(local.recalled_memories);
        } else if (agent_options.remember_interaction) {
            stored_memory_id = agent.remember(
                "Usuário: " + message + "\nAssistente: " + answer);
        }

        if (stored_memory_id != 0U && !options_.memory_path.empty()) {
            memory_.save(options_.memory_path);
        }
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();

        std::ostringstream body;
        body << "{\"ok\":true,\"reply\":" << json_escape(answer)
             << ",\"provider\":" << json_escape(provider)
             << ",\"provider_model\":" << json_escape(provider_model)
             << ",\"generated_tokens\":" << generated_tokens
             << ",\"memory_id\":" << stored_memory_id
             << ",\"latency_ms\":" << elapsed_ms
             << ",\"internet_source\":" << json_escape(internet_result.source_url)
             << ",\"warning\":" << json_escape(warning)
             << ",\"recalled\":[";
        for (std::size_t index = 0U; index < recalled.size(); ++index) {
            if (index != 0U) body << ',';
            const MemoryHit& hit = recalled[index];
            body << "{\"id\":" << hit.id << ",\"score\":"
                 << std::setprecision(7) << hit.score << ",\"text\":"
                 << json_escape(hit.text.substr(0U, 1'024U)) << '}';
        }
        body << "]}";
        return HttpResponse{200, "application/json; charset=utf-8", body.str(), {}};
    }

    [[nodiscard]] HttpResponse code_proposal_response(std::string_view encoded) {
        if (!self_editor_.enabled()) {
            throw HttpFailure(422, "autoedição desativada; reinicie com --enable-self-edit");
        }
        const JsonObject object = JsonParser(encoded).parse_object();
        const std::string file = required_json_string(object, "file");
        const std::string instruction = required_json_string(object, "instruction");
        CodeProposal proposal = self_editor_.propose(file, instruction);
        std::ostringstream body;
        body << "{\"ok\":true,\"proposal_id\":" << proposal.id
             << ",\"file\":" << json_escape(proposal.relative_path)
             << ",\"replacement\":" << json_escape(proposal.replacement)
             << ",\"confirmation\":" << json_escape("APLICAR " + std::to_string(proposal.id))
             << '}';
        return HttpResponse{200, "application/json; charset=utf-8", body.str(), {}};
    }

    [[nodiscard]] HttpResponse code_apply_response(std::string_view encoded) {
        if (!self_editor_.enabled()) {
            throw HttpFailure(422, "autoedição desativada; reinicie com --enable-self-edit");
        }
        const JsonObject object = JsonParser(encoded).parse_object();
        const std::size_t proposal_id = bounded_integer(
            optional_json_number(object, "proposal_id", 0.0), 1U,
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()), "proposal_id");
        const std::string confirmation = required_json_string(object, "confirmation");
        AppliedCodeChange applied = self_editor_.apply(
            static_cast<std::uint64_t>(proposal_id), confirmation);
        std::ostringstream body;
        body << "{\"ok\":true,\"proposal_id\":" << applied.proposal_id
             << ",\"changed_file\":" << json_escape(applied.changed_file.string())
             << ",\"backup_file\":" << json_escape(applied.backup_file.string())
             << ",\"restart_required\":true}";
        return HttpResponse{200, "application/json; charset=utf-8", body.str(), {}};
    }

    [[nodiscard]] HttpResponse remember_response(std::string_view encoded) {
        const JsonObject object = JsonParser(encoded).parse_object();
        std::string text = required_json_string(object, "text");
        const std::string_view trimmed = trim_ascii(text);
        text.assign(trimmed.begin(), trimmed.end());
        text = sanitize_utf8(text);
        if (text.empty() || text.size() > maximum_message_bytes) {
            throw HttpFailure(422, "text deve conter entre 1 e 16000 bytes");
        }
        CognitiveAgent agent(tokenizer_, model_, memory_);
        const std::uint64_t id = agent.remember(std::move(text));
        if (!options_.memory_path.empty()) {
            memory_.save(options_.memory_path);
        }
        return HttpResponse{200, "application/json; charset=utf-8",
                            "{\"ok\":true,\"id\":" + std::to_string(id) + "}", {}};
    }

    const BpeTokenizer& tokenizer_;
    const NeuralLanguageModel& model_;
    LongTermMemory& memory_;
    WebServerOptions options_;
    LanguageApiClient language_api_;
    InternetSearch internet_;
    SafeSelfEditor self_editor_;
    std::atomic<std::uint64_t> request_sequence_{1U};
};

void handle_connection(NativeSocket socket,
                       WebRuntime& runtime,
                       std::size_t maximum_request_bytes,
                       std::mutex& log_mutex) noexcept {
    SocketGuard client(socket);
    try {
        const HttpRequest request = read_http_request(socket, maximum_request_bytes);
        write_http_response(socket, runtime.route(request));
    } catch (const HttpFailure& failure) {
        try {
            write_http_response(socket, json_error_response(failure.status(), failure.what()));
        } catch (...) {
        }
    } catch (const std::exception& exception) {
        try {
            write_http_response(socket, json_error_response(500, exception.what()));
        } catch (...) {
        }
        std::lock_guard lock(log_mutex);
        std::cerr << "web: " << exception.what() << '\n';
    }
}

} // namespace

void run_web_chat_server(const BpeTokenizer& tokenizer,
                         const NeuralLanguageModel& model,
                         LongTermMemory& memory,
                         const WebServerOptions& options) {
    if (options.port == 0U) {
        throw Error("a porta HTTP deve estar entre 1 e 65535");
    }
    if (options.worker_count == 0U || options.worker_count > 64U) {
        throw Error("worker_count HTTP deve estar entre 1 e 64");
    }
    if (options.maximum_request_bytes < 1'024U
        || options.maximum_request_bytes > 1024U * 1024U) {
        throw Error("maximum_request_bytes deve estar entre 1 KiB e 1 MiB");
    }
    if (options.self_edit_enabled && options.bind_address != "127.0.0.1") {
        throw Error("autoedição só pode ser vinculada a 127.0.0.1");
    }

    SocketEnvironment environment;
    SocketGuard listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener.get() == invalid_socket) {
        throw Error("não foi possível criar socket HTTP");
    }

    int reuse = 1;
#ifdef _WIN32
    setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (inet_pton(AF_INET, options.bind_address.c_str(), &address.sin_addr) != 1) {
        throw Error("endereço IPv4 inválido para --host: " + options.bind_address);
    }
    if (bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        throw Error("não foi possível vincular " + options.bind_address + ':'
                    + std::to_string(options.port) + " (erro "
                    + std::to_string(last_socket_error()) + ')');
    }
    if (listen(listener.get(), SOMAXCONN) != 0) {
        throw Error("listen falhou no servidor HTTP");
    }

    WebRuntime runtime(tokenizer, model, memory, options);
    std::deque<NativeSocket> queue;
    std::mutex queue_mutex;
    std::mutex log_mutex;
    std::condition_variable queue_condition;
    bool stopping = false;
    std::vector<std::thread> workers;
    workers.reserve(options.worker_count);

    for (std::size_t index = 0U; index < options.worker_count; ++index) {
        workers.emplace_back([&]() {
            while (true) {
                NativeSocket client = invalid_socket;
                {
                    std::unique_lock lock(queue_mutex);
                    queue_condition.wait(lock, [&]() { return stopping || !queue.empty(); });
                    if (stopping && queue.empty()) {
                        return;
                    }
                    client = queue.front();
                    queue.pop_front();
                }
                handle_connection(client, runtime, options.maximum_request_bytes, log_mutex);
            }
        });
    }

    std::cout << "Interface Cortex disponível em http://" << options.bind_address
              << ':' << options.port << "\n"
              << "Workers HTTP: " << options.worker_count
              << " | modelo ativo: "
              << (runtime.language_api_configured()
                      ? (options.language_api.display_model.empty()
                             ? options.language_api.model : options.language_api.display_model)
                      : "GRU local (fallback)")
              << " | internet: " << (options.internet_enabled ? "habilitada" : "desabilitada")
              << " | Ctrl+C para encerrar\n" << std::flush;

    try {
        while (true) {
            sockaddr_in client_address{};
#ifdef _WIN32
            int client_length = sizeof(client_address);
#else
            socklen_t client_length = sizeof(client_address);
#endif
            const NativeSocket accepted = accept(
                listener.get(), reinterpret_cast<sockaddr*>(&client_address), &client_length);
            if (accepted == invalid_socket) {
                const int error = last_socket_error();
                if (interrupted_socket_error(error)) {
                    continue;
                }
                throw Error("accept falhou no servidor HTTP (erro "
                            + std::to_string(error) + ')');
            }
            configure_client_socket(accepted);

            bool queued = false;
            {
                std::lock_guard lock(queue_mutex);
                if (queue.size() < maximum_queued_connections) {
                    queue.push_back(accepted);
                    queued = true;
                }
            }
            if (queued) {
                queue_condition.notify_one();
            } else {
                try {
                    write_http_response(accepted,
                                        json_error_response(503, "servidor temporariamente ocupado"));
                } catch (...) {
                }
                close_socket(accepted);
            }
        }
    } catch (...) {
        {
            std::lock_guard lock(queue_mutex);
            stopping = true;
            for (const NativeSocket queued : queue) {
                close_socket(queued);
            }
            queue.clear();
        }
        queue_condition.notify_all();
        for (std::thread& worker : workers) {
            worker.join();
        }
        throw;
    }
}

} // namespace cortex
