#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace cortex {

// TokenId fixo evita diferenças de ABI entre plataformas de 32 e 64 bits.
using TokenId = std::uint32_t;

// Exceção única do domínio torna erros de modelo/dados distinguíveis de falhas do SO.
class Error final : public std::runtime_error {
public:
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

// Gerador pequeno e determinístico usado na inicialização e na amostragem do modelo.
class Random final {
public:
    explicit Random(std::uint64_t seed = 0xC07E'CAFE'1234'5678ULL);

    [[nodiscard]] std::uint64_t next_u64();
    [[nodiscard]] float uniform();
    [[nodiscard]] float uniform(float low, float high);
    [[nodiscard]] float normal(float mean = 0.0F, float standard_deviation = 1.0F);

private:
    std::uint64_t state_;
    bool has_spare_normal_ = false;
    float spare_normal_ = 0.0F;
};

// Pool persistente: threads são criadas uma vez, não a cada produto matricial.
class ThreadPool final {
public:
    explicit ThreadPool(std::size_t worker_count = default_worker_count());
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] static std::size_t default_worker_count();
    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

    // Empacota qualquer callable em uma tarefa e devolve seu future tipado.
    template <class Function>
    auto submit(Function&& function) -> std::future<std::invoke_result_t<Function>> {
        using Result = std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        std::future<Result> future = task->get_future();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                throw Error("tentativa de enviar trabalho para um ThreadPool encerrado");
            }
            tasks_.emplace_back([task]() { (*task)(); });
        }
        condition_.notify_one();
        return future;
    }

    // Divide [begin,end) em blocos; o chamador também trabalha para reduzir latência.
    template <class Function>
    void parallel_for(std::size_t begin,
                      std::size_t end,
                      std::size_t minimum_grain,
                      Function&& function) {
        if (end <= begin) {
            return;
        }
        const std::size_t amount = end - begin;
        if (workers_.empty() || amount <= minimum_grain) {
            function(begin, end);
            return;
        }

        const std::size_t possible_chunks = (amount + minimum_grain - 1U) / minimum_grain;
        const std::size_t chunks = std::min(workers_.size() + 1U, possible_chunks);
        if (chunks <= 1U) {
            function(begin, end);
            return;
        }

        const std::size_t block = (amount + chunks - 1U) / chunks;
        std::vector<std::future<void>> futures;
        futures.reserve(chunks - 1U);

        // Os blocos remotos começam no segundo bloco; o primeiro fica no chamador.
        for (std::size_t chunk = 1U; chunk < chunks; ++chunk) {
            const std::size_t chunk_begin = begin + chunk * block;
            const std::size_t chunk_end = std::min(end, chunk_begin + block);
            if (chunk_begin >= chunk_end) {
                break;
            }
            futures.emplace_back(submit([chunk_begin, chunk_end, &function]() {
                function(chunk_begin, chunk_end);
            }));
        }

        function(begin, std::min(end, begin + block));
        for (auto& future : futures) {
            future.get();
        }
    }

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
};

// Instância compartilhada por inferência, treinamento e busca vetorial.
ThreadPool& global_thread_pool();

// Matriz row-major contígua para maximizar localidade e permitir autovetorização do C++.
class Matrix final {
public:
    Matrix() = default;
    Matrix(std::size_t rows, std::size_t columns, float initial_value = 0.0F);

    [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t columns() const noexcept { return columns_; }
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

    [[nodiscard]] float& operator()(std::size_t row, std::size_t column);
    [[nodiscard]] const float& operator()(std::size_t row, std::size_t column) const;
    [[nodiscard]] std::span<float> row(std::size_t row_index);
    [[nodiscard]] std::span<const float> row(std::size_t row_index) const;
    [[nodiscard]] std::vector<float>& values() noexcept { return values_; }
    [[nodiscard]] const std::vector<float>& values() const noexcept { return values_; }

    void fill(float value);
    void initialize_xavier(Random& random, std::size_t fan_in, std::size_t fan_out);
    void matvec(std::span<const float> input,
                std::span<float> output,
                bool add = false) const;
    void transposed_matvec(std::span<const float> input,
                           std::span<float> output,
                           bool add = false) const;

private:
    std::size_t rows_ = 0U;
    std::size_t columns_ = 0U;
    std::vector<float> values_;
};

// Cada parâmetro mantém pesos, gradiente e momentos do Adam em buffers contíguos.
struct Parameter final {
    std::string name;
    Matrix value;
    std::vector<float> gradient;
    std::vector<float> first_moment;
    std::vector<float> second_moment;

    Parameter() = default;
    Parameter(std::string parameter_name, std::size_t rows, std::size_t columns);

    void zero_gradient();
    [[nodiscard]] std::size_t size() const noexcept { return value.size(); }
};

// Primitivas matemáticas explícitas — nenhuma biblioteca neural de caixa-preta é usada.
[[nodiscard]] float stable_sigmoid(float value) noexcept;
void add_outer_product(Parameter& target,
                       std::span<const float> left,
                       std::span<const float> right,
                       float scale = 1.0F);
[[nodiscard]] float dot_product(std::span<const float> left,
                                std::span<const float> right);
[[nodiscard]] float l2_norm(std::span<const float> values);
void normalize_in_place(std::span<float> values);

// Escrita little-endian explícita deixa checkpoints independentes do compilador/ABI.
class BinaryWriter final {
public:
    explicit BinaryWriter(const std::filesystem::path& path);

    void bytes(std::span<const std::byte> data);
    void magic(std::string_view value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void f32(float value);
    void string(std::string_view value);
    void flush();

private:
    std::ofstream stream_;
};

// Leitor correspondente aplica limites antes de alocar memória proveniente do arquivo.
class BinaryReader final {
public:
    explicit BinaryReader(const std::filesystem::path& path);

    void expect_magic(std::string_view expected);
    [[nodiscard]] std::uint32_t u32();
    [[nodiscard]] std::uint64_t u64();
    [[nodiscard]] float f32();
    [[nodiscard]] std::string string(std::size_t maximum_size);
    void require_end();

private:
    void read_exact(std::span<std::byte> destination);

    std::ifstream stream_;
};

// Persistência usa arquivo temporário no mesmo diretório e troca final por rename.
[[nodiscard]] std::filesystem::path temporary_path_for(const std::filesystem::path& target);
void replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& target);

} // namespace cortex
