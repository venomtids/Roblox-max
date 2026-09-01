#include "cortex/core.hpp"
#include "cortex/memory.hpp"
#include "cortex/neural_model.hpp"
#include "cortex/tokenizer.hpp"
#include "cortex/web_server.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
std::size_t passed = 0U;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw cortex::Error("teste falhou: " + message);
    }
    ++passed;
}

[[nodiscard]] bool near(float left, float right, float tolerance = 1.0e-5F) {
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] fs::path temporary_test_directory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path directory = fs::temp_directory_path()
                         / ("cortex-native-tests-" + std::to_string(stamp));
    fs::create_directories(directory);
    return directory;
}

void test_matrix() {
    cortex::Matrix matrix(2U, 3U);
    matrix(0U, 0U) = 1.0F;
    matrix(0U, 1U) = 2.0F;
    matrix(0U, 2U) = 3.0F;
    matrix(1U, 0U) = -1.0F;
    matrix(1U, 1U) = 0.5F;
    matrix(1U, 2U) = 2.0F;
    const std::vector<float> input{2.0F, -1.0F, 4.0F};
    std::vector<float> output(2U, 0.0F);
    matrix.matvec(input, output);
    expect(near(output[0], 12.0F), "produto matriz-vetor, linha zero");
    expect(near(output[1], 5.5F), "produto matriz-vetor, linha um");

    std::vector<float> transposed(3U, 0.0F);
    matrix.transposed_matvec(std::vector<float>{2.0F, 3.0F}, transposed);
    expect(near(transposed[0], -1.0F), "produto transposto, coluna zero");
    expect(near(transposed[1], 5.5F), "produto transposto, coluna um");
    expect(near(transposed[2], 12.0F), "produto transposto, coluna dois");
}

void test_tokenizer(const fs::path& directory) {
    const std::string text = "ação, banana e UTF-8: çã 🚀\nbanana banana";
    cortex::BpeTokenizer tokenizer;
    tokenizer.train(text, 48U, 2U);
    const std::vector<cortex::TokenId> encoded = tokenizer.encode(text, true, true);
    expect(encoded.front() == cortex::BpeTokenizer::bos_id, "BOS foi inserido");
    expect(encoded.back() == cortex::BpeTokenizer::eos_id, "EOS foi inserido");
    expect(tokenizer.decode(encoded) == text, "BPE reconstrói bytes UTF-8 exatamente");
    expect(tokenizer.merge_count() > 0U, "BPE aprendeu pelo menos um merge");

    const fs::path file = directory / "tokenizer.cbpe";
    tokenizer.save(file);
    const cortex::BpeTokenizer loaded = cortex::BpeTokenizer::load(file);
    expect(loaded.encode(text, true, true) == encoded, "checkpoint BPE preserva ranks");
    expect(loaded.decode(encoded) == text, "tokenizador carregado decodifica igualmente");
}

void test_neural_training_and_checkpoint(const fs::path& directory) {
    cortex::ModelConfig config;
    config.vocabulary_size = cortex::BpeTokenizer::first_merge_id;
    config.embedding_size = 8U;
    config.hidden_size = 12U;
    config.layer_count = 1U;
    config.seed = 12345U;
    cortex::NeuralLanguageModel model(config);

    // Linguagem sintética alterna dois tokens; uma GRU deve aprender a transição rapidamente.
    std::vector<cortex::TokenId> sequence;
    sequence.push_back(cortex::BpeTokenizer::bos_id);
    for (std::size_t index = 0U; index < 96U; ++index) {
        sequence.push_back(index % 2U == 0U ? 10U : 11U);
    }
    sequence.push_back(cortex::BpeTokenizer::eos_id);

    const float before = model.evaluate_loss(sequence);
    cortex::TrainingOptions options;
    options.epochs = 18U;
    options.sequence_length = 16U;
    options.learning_rate = 0.02F;
    options.gradient_clip = 5.0F;
    const std::vector<cortex::TrainingMetrics> history = model.train(sequence, options);
    const float after = model.evaluate_loss(sequence);
    expect(history.size() == options.epochs, "histórico contém todas as épocas");
    expect(std::isfinite(after), "loss treinada é finita");
    expect(after < before * 0.45F, "BPTT e Adam reduzem significativamente a loss");

    cortex::NeuralLanguageModel::State original_state = model.zero_state();
    const std::vector<float> original_logits = model.step(10U, original_state);
    const fs::path file = directory / "model.cgru";
    model.save(file);
    cortex::NeuralLanguageModel loaded = cortex::NeuralLanguageModel::load(file);
    cortex::NeuralLanguageModel::State loaded_state = loaded.zero_state();
    const std::vector<float> loaded_logits = loaded.step(10U, loaded_state);
    expect(original_logits == loaded_logits, "checkpoint preserva pesos float bit a bit");
    expect(loaded.optimizer_step() == model.optimizer_step(),
           "checkpoint preserva passo e momentos do Adam");

    cortex::GenerationOptions generation;
    generation.maximum_tokens = 8U;
    generation.top_k = 2U;
    generation.temperature = 0.5F;
    generation.seed = 7U;
    const std::vector<cortex::TokenId> first = model.generate(
        std::vector<cortex::TokenId>{10U}, generation);
    const std::vector<cortex::TokenId> second = loaded.generate(
        std::vector<cortex::TokenId>{10U}, generation);
    expect(first == second, "amostragem com mesma semente é determinística");
}

void test_direct_answer_extraction() {
    expect(cortex::extract_direct_answer(
               "  Assistente: Resposta objetiva.\n\nUsuário: próxima pergunta")
               == "Resposta objetiva.",
           "agente remove papel e interrompe antes do próximo usuário");
    expect(cortex::extract_direct_answer("Resposta curta.\nSistema: novo turno")
               == "Resposta curta.",
           "agente interrompe antes de uma continuação de sistema");
    expect(cortex::extract_direct_answer("Cortex: texto sem marcador posterior  ")
               == "texto sem marcador posterior",
           "agente remove prefixo Cortex e espaços finais");
    expect(cortex::extract_direct_answer("\n\t") == "",
           "saída neural sem conteúdo permanece vazia para fallback");
}

void test_embedded_web_interface() {
    const std::string_view html = cortex::web_chat_html();
    expect(html.starts_with("<!doctype html>"), "interface web possui documento HTML completo");
    expect(html.find("/api/chat") != std::string_view::npos,
           "interface web conversa com endpoint relativo nativo");
    expect(html.find("Node.js") != std::string_view::npos,
           "interface informa ausência de runtime Node.js");
}

void test_long_term_memory(const fs::path& directory) {
    cortex::LongTermMemory memory(3U);
    const std::uint64_t alpha = memory.remember("alfa", std::vector<float>{1.0F, 0.0F, 0.0F});
    const std::uint64_t beta = memory.remember("beta", std::vector<float>{0.0F, 1.0F, 0.0F});
    expect(alpha != beta, "ids de memória são únicos");

    const std::vector<cortex::MemoryHit> hits = memory.search(
        std::vector<float>{0.9F, 0.1F, 0.0F}, 2U);
    expect(hits.size() == 2U, "busca retorna limite solicitado");
    expect(hits.front().id == alpha, "cosseno ordena o vetor semanticamente mais próximo");
    expect(hits.front().score > hits.back().score, "scores possuem ordem decrescente");

    const fs::path file = directory / "memory.cmem";
    memory.save(file);
    cortex::LongTermMemory loaded = cortex::LongTermMemory::load(file, 3U);
    expect(loaded.size() == 2U, "persistência conserva todos os episódios");
    const std::vector<cortex::MemoryHit> loaded_hits = loaded.search(
        std::vector<float>{0.0F, 1.0F, 0.0F}, 1U);
    expect(loaded_hits.front().id == beta, "índices persistidos mantêm semântica");
    expect(loaded.erase(alpha), "remoção encontra id existente");
    expect(!loaded.erase(alpha), "remoção sinaliza id já ausente");
}

} // namespace

int main() {
    const fs::path directory = temporary_test_directory();
    try {
        test_matrix();
        test_tokenizer(directory);
        test_neural_training_and_checkpoint(directory);
        test_direct_answer_extraction();
        test_embedded_web_interface();
        test_long_term_memory(directory);
        fs::remove_all(directory);
        std::cout << "OK — " << passed << " verificações passaram.\n";
        return 0;
    } catch (const std::exception& exception) {
        fs::remove_all(directory);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
