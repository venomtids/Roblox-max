#include "cortex/core.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cortex {

Random::Random(std::uint64_t seed)
    : state_(seed == 0U ? 0x9E37'79B9'7F4A'7C15ULL : seed) {}

std::uint64_t Random::next_u64() {
    // xorshift64* tem estado mínimo, período 2^64-1 e caminho extremamente curto.
    std::uint64_t value = state_;
    value ^= value >> 12U;
    value ^= value << 25U;
    value ^= value >> 27U;
    state_ = value;
    return value * 0x2545'F491'4F6C'DD1DULL;
}

float Random::uniform() {
    // Os 24 bits altos cabem exatamente na mantissa de um float IEEE-754.
    constexpr float inverse = 1.0F / static_cast<float>(1U << 24U);
    return static_cast<float>(next_u64() >> 40U) * inverse;
}

float Random::uniform(float low, float high) {
    return low + (high - low) * uniform();
}

float Random::normal(float mean, float standard_deviation) {
    // Box-Muller produz dois gaussianos; o segundo é guardado para a próxima chamada.
    if (has_spare_normal_) {
        has_spare_normal_ = false;
        return mean + standard_deviation * spare_normal_;
    }
    const float first = std::max(uniform(), 1.0e-7F);
    const float second = uniform();
    const float radius = std::sqrt(-2.0F * std::log(first));
    constexpr float two_pi = 6.28318530717958647692F;
    const float angle = two_pi * second;
    spare_normal_ = radius * std::sin(angle);
    has_spare_normal_ = true;
    return mean + standard_deviation * radius * std::cos(angle);
}

std::size_t ThreadPool::default_worker_count() {
    const std::size_t detected = static_cast<std::size_t>(std::thread::hardware_concurrency());
    // Um núcleo fica para o chamador, que participa de parallel_for.
    return detected > 1U ? detected - 1U : 1U;
}

ThreadPool::ThreadPool(std::size_t worker_count) {
    workers_.reserve(worker_count);
    for (std::size_t index = 0U; index < worker_count; ++index) {
        workers_.emplace_back([this]() { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

ThreadPool& global_thread_pool() {
    static ThreadPool pool;
    return pool;
}

Matrix::Matrix(std::size_t rows, std::size_t columns, float initial_value)
    : rows_(rows), columns_(columns) {
    if (columns != 0U && rows > std::numeric_limits<std::size_t>::max() / columns) {
        throw Error("dimensões de matriz excedem o espaço endereçável");
    }
    values_.assign(rows * columns, initial_value);
}

float& Matrix::operator()(std::size_t row_index, std::size_t column_index) {
    if (row_index >= rows_ || column_index >= columns_) {
        throw Error("índice fora dos limites da matriz");
    }
    return values_[row_index * columns_ + column_index];
}

const float& Matrix::operator()(std::size_t row_index, std::size_t column_index) const {
    if (row_index >= rows_ || column_index >= columns_) {
        throw Error("índice fora dos limites da matriz");
    }
    return values_[row_index * columns_ + column_index];
}

std::span<float> Matrix::row(std::size_t row_index) {
    if (row_index >= rows_) {
        throw Error("linha fora dos limites da matriz");
    }
    return std::span<float>(values_.data() + row_index * columns_, columns_);
}

std::span<const float> Matrix::row(std::size_t row_index) const {
    if (row_index >= rows_) {
        throw Error("linha fora dos limites da matriz");
    }
    return std::span<const float>(values_.data() + row_index * columns_, columns_);
}

void Matrix::fill(float value) {
    std::fill(values_.begin(), values_.end(), value);
}

void Matrix::initialize_xavier(Random& random, std::size_t fan_in, std::size_t fan_out) {
    if (fan_in + fan_out == 0U) {
        throw Error("Xavier exige fan_in ou fan_out não nulo");
    }
    const float limit = std::sqrt(6.0F / static_cast<float>(fan_in + fan_out));
    for (float& value : values_) {
        value = random.uniform(-limit, limit);
    }
}

void Matrix::matvec(std::span<const float> input,
                    std::span<float> output,
                    bool add) const {
    if (input.size() != columns_ || output.size() != rows_) {
        throw Error("dimensões incompatíveis em Matrix::matvec");
    }
    if (!add) {
        std::fill(output.begin(), output.end(), 0.0F);
    }

    auto calculate_rows = [this, input, output](std::size_t begin, std::size_t end) {
        for (std::size_t row_index = begin; row_index < end; ++row_index) {
            const float* weights = values_.data() + row_index * columns_;
            float sum = 0.0F;
            // Loop contíguo deliberadamente simples: -O3 o vetoriza com SIMD.
            for (std::size_t column = 0U; column < columns_; ++column) {
                sum += weights[column] * input[column];
            }
            output[row_index] += sum;
        }
    };

    const std::size_t operations = rows_ * columns_;
    if (operations >= 65'536U && rows_ >= 8U) {
        global_thread_pool().parallel_for(0U, rows_, 8U, calculate_rows);
    } else {
        calculate_rows(0U, rows_);
    }
}

void Matrix::transposed_matvec(std::span<const float> input,
                               std::span<float> output,
                               bool add) const {
    if (input.size() != rows_ || output.size() != columns_) {
        throw Error("dimensões incompatíveis em Matrix::transposed_matvec");
    }
    if (!add) {
        std::fill(output.begin(), output.end(), 0.0F);
    }

    // Cada tarefa possui colunas exclusivas, portanto não há atomics nem falso compartilhamento.
    auto calculate_columns = [this, input, output](std::size_t begin, std::size_t end) {
        for (std::size_t column = begin; column < end; ++column) {
            float sum = 0.0F;
            for (std::size_t row_index = 0U; row_index < rows_; ++row_index) {
                sum += values_[row_index * columns_ + column] * input[row_index];
            }
            output[column] += sum;
        }
    };

    const std::size_t operations = rows_ * columns_;
    if (operations >= 65'536U && columns_ >= 8U) {
        global_thread_pool().parallel_for(0U, columns_, 8U, calculate_columns);
    } else {
        calculate_columns(0U, columns_);
    }
}

Parameter::Parameter(std::string parameter_name, std::size_t rows, std::size_t columns)
    : name(std::move(parameter_name)),
      value(rows, columns),
      gradient(value.size(), 0.0F),
      first_moment(value.size(), 0.0F),
      second_moment(value.size(), 0.0F) {}

void Parameter::zero_gradient() {
    std::fill(gradient.begin(), gradient.end(), 0.0F);
}

float stable_sigmoid(float value) noexcept {
    // As duas formas evitam exp(+valor) quando o módulo é muito grande.
    if (value >= 0.0F) {
        const float exponential = std::exp(-value);
        return 1.0F / (1.0F + exponential);
    }
    const float exponential = std::exp(value);
    return exponential / (1.0F + exponential);
}

void add_outer_product(Parameter& target,
                       std::span<const float> left,
                       std::span<const float> right,
                       float scale) {
    if (target.value.rows() != left.size() || target.value.columns() != right.size()) {
        throw Error("dimensões incompatíveis no produto externo");
    }
    const std::size_t columns = right.size();
    for (std::size_t row_index = 0U; row_index < left.size(); ++row_index) {
        const float left_scaled = left[row_index] * scale;
        float* destination = target.gradient.data() + row_index * columns;
        for (std::size_t column = 0U; column < columns; ++column) {
            destination[column] += left_scaled * right[column];
        }
    }
}

float dot_product(std::span<const float> left, std::span<const float> right) {
    if (left.size() != right.size()) {
        throw Error("vetores de tamanhos diferentes no produto escalar");
    }
    float sum = 0.0F;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        sum += left[index] * right[index];
    }
    return sum;
}

float l2_norm(std::span<const float> values) {
    return std::sqrt(std::max(0.0F, dot_product(values, values)));
}

void normalize_in_place(std::span<float> values) {
    const float norm = l2_norm(values);
    if (norm <= 1.0e-12F) {
        return;
    }
    const float inverse = 1.0F / norm;
    for (float& value : values) {
        value *= inverse;
    }
}

BinaryWriter::BinaryWriter(const std::filesystem::path& path)
    : stream_(path, std::ios::binary | std::ios::trunc) {
    if (!stream_) {
        throw Error("não foi possível abrir para escrita: " + path.string());
    }
}

void BinaryWriter::bytes(std::span<const std::byte> data) {
    stream_.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    if (!stream_) {
        throw Error("falha ao escrever arquivo binário");
    }
}

void BinaryWriter::magic(std::string_view value) {
    bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                     value.size()));
}

void BinaryWriter::u32(std::uint32_t value) {
    std::array<std::byte, 4U> encoded{};
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        encoded[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
    bytes(encoded);
}

void BinaryWriter::u64(std::uint64_t value) {
    std::array<std::byte, 8U> encoded{};
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        encoded[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
    bytes(encoded);
}

void BinaryWriter::f32(float value) {
    u32(std::bit_cast<std::uint32_t>(value));
}

void BinaryWriter::string(std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw Error("string grande demais para serialização");
    }
    u32(static_cast<std::uint32_t>(value.size()));
    bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                     value.size()));
}

void BinaryWriter::flush() {
    stream_.flush();
    if (!stream_) {
        throw Error("falha ao sincronizar arquivo binário");
    }
}

void BinaryWriter::close() {
    if (stream_.is_open()) {
        stream_.close();
        if (stream_.fail()) {
            throw Error("falha ao fechar arquivo binário");
        }
    }
}

BinaryReader::BinaryReader(const std::filesystem::path& path)
    : stream_(path, std::ios::binary) {
    if (!stream_) {
        throw Error("não foi possível abrir para leitura: " + path.string());
    }
}

void BinaryReader::read_exact(std::span<std::byte> destination) {
    stream_.read(reinterpret_cast<char*>(destination.data()),
                 static_cast<std::streamsize>(destination.size()));
    if (stream_.gcount() != static_cast<std::streamsize>(destination.size())) {
        throw Error("arquivo binário truncado ou corrompido");
    }
}

void BinaryReader::expect_magic(std::string_view expected) {
    std::vector<std::byte> found(expected.size());
    read_exact(found);
    const auto expected_bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(expected.data()), expected.size());
    if (!std::equal(found.begin(), found.end(), expected_bytes.begin(), expected_bytes.end())) {
        throw Error("assinatura de arquivo inválida");
    }
}

std::uint32_t BinaryReader::u32() {
    std::array<std::byte, 4U> encoded{};
    read_exact(encoded);
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned int>(encoded[index]))
                 << (index * 8U);
    }
    return value;
}

std::uint64_t BinaryReader::u64() {
    std::array<std::byte, 8U> encoded{};
    read_exact(encoded);
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned int>(encoded[index]))
                 << (index * 8U);
    }
    return value;
}

float BinaryReader::f32() {
    return std::bit_cast<float>(u32());
}

std::string BinaryReader::string(std::size_t maximum_size) {
    const std::size_t size = static_cast<std::size_t>(u32());
    if (size > maximum_size) {
        throw Error("string serializada excede o limite de segurança");
    }
    std::string result(size, '\0');
    read_exact(std::span<std::byte>(reinterpret_cast<std::byte*>(result.data()), result.size()));
    return result;
}

void BinaryReader::require_end() {
    if (stream_.peek() != std::char_traits<char>::eof()) {
        throw Error("dados inesperados após o fim lógico do arquivo");
    }
}

std::filesystem::path temporary_path_for(const std::filesystem::path& target) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::filesystem::path temporary = target;
    temporary += ".tmp." + std::to_string(now) + "." + std::to_string(thread_hash);
    if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path());
    }
    return temporary;
}

void replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& target) {
    std::error_code error;
    // POSIX substitui atomicamente um destino existente com rename.
    std::filesystem::rename(temporary, target, error);
    if (!error) {
        return;
    }

#ifdef _WIN32
    // MinGW/Windows pode recusar std::filesystem::rename quando o destino existe
    // ou quando o handle ainda não foi liberado (antivírus, indexer ou handle
    // ainda aberto). O arquivo temporário já foi fechado antes de chegar aqui,
    // mas o SO pode manter o destino bloqueado por um instante (Defender).
    // Trata ERROR_ACCESS_DENIED (5) e ERROR_SHARING_VIOLATION (32) com retry
    // exponencial e tenta ReplaceFileW / CopyFileW como fallback.
    const auto wtmp = temporary.wstring();
    const auto wtgt = target.wstring();
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (::MoveFileExW(wtmp.c_str(), wtgt.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            return;
        }
        error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        if (error.value() != 5 && error.value() != 32) {
            // Erro diferente: tenta ReplaceFileW antes de desistir
            if (::ReplaceFileW(wtgt.c_str(), wtmp.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH,
                               nullptr, nullptr) != 0) {
                // ReplaceFileW moveu temp para target e criou backup nulo
                return;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
        // Na metade das tentativas, tenta remover o destino se ele estiver travado
        if (attempt == 4 || attempt == 7) {
            std::error_code remove_error;
            std::filesystem::remove(target, remove_error);
            if (!remove_error) {
                std::filesystem::rename(temporary, target, error);
                if (!error) return;
                if (::MoveFileExW(wtmp.c_str(), wtgt.c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
                    return;
                }
            }
        }
    }
    // Fallback final: CopyFileW + Delete
    if (::CopyFileW(wtmp.c_str(), wtgt.c_str(), FALSE) != 0) {
        std::error_code rem;
        std::filesystem::remove(temporary, rem);
        return;
    }
    error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
    // Tenta ReplaceFileW uma última vez antes de falhar
    if (::ReplaceFileW(wtgt.c_str(), wtmp.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH,
                       nullptr, nullptr) != 0) {
        return;
    }
#endif
    std::filesystem::remove(temporary);
    throw Error("não foi possível publicar '" + temporary.string() + "' -> '" + target.string()
                + "': " + error.message() + " (" + std::to_string(error.value()) + ")");
}

} // namespace cortex
