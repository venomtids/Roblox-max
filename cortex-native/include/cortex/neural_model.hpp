#pragma once

#include "cortex/core.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace cortex {

// Dimensões serializadas junto aos pesos; limites são validados antes de alocar.
struct ModelConfig final {
    std::size_t vocabulary_size = 0U;
    std::size_t embedding_size = 64U;
    std::size_t hidden_size = 128U;
    std::size_t layer_count = 2U;
    std::uint64_t seed = 0xA17E'2026ULL;
};

struct TrainingOptions final {
    std::size_t epochs = 30U;
    std::size_t sequence_length = 64U;
    float learning_rate = 0.003F;
    float gradient_clip = 1.0F;
    float beta1 = 0.9F;
    float beta2 = 0.999F;
    float epsilon = 1.0e-8F;
};

struct TrainingMetrics final {
    std::size_t epoch = 0U;
    float average_loss = 0.0F;
    float perplexity = 0.0F;
    std::size_t predicted_tokens = 0U;
};

struct GenerationOptions final {
    std::size_t maximum_tokens = 96U;
    std::size_t top_k = 8U;
    float temperature = 0.6F;
    float repetition_penalty = 1.08F;
    std::uint64_t seed = 0x51A9'B00BULL;
};

// Language model GRU empilhado, treinável por BPTT e Adam implementados neste projeto.
class NeuralLanguageModel final {
public:
    // Um estado contém um vetor oculto por camada e pode viver por toda uma sessão.
    using State = std::vector<std::vector<float>>;

    explicit NeuralLanguageModel(ModelConfig config);

    [[nodiscard]] const ModelConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::size_t parameter_count() const noexcept;
    [[nodiscard]] std::uint64_t optimizer_step() const noexcept { return optimizer_step_; }
    [[nodiscard]] State zero_state() const;

    // Consome um token e devolve logits para o token seguinte.
    [[nodiscard]] std::vector<float> step(TokenId token, State& state) const;

    // BPTT truncado atualiza todos os gates, embeddings e projeção de saída.
    std::vector<TrainingMetrics> train(
        std::span<const TokenId> tokens,
        const TrainingOptions& options,
        const std::function<void(const TrainingMetrics&)>& on_epoch = {});

    [[nodiscard]] float evaluate_loss(std::span<const TokenId> tokens) const;

    // Geração autoregressiva com top-k, temperatura e penalidade de repetição.
    [[nodiscard]] std::vector<TokenId> generate(
        std::span<const TokenId> prompt,
        const GenerationOptions& options) const;

    // Média normalizada dos estados ocultos: representação usada pela memória semântica.
    [[nodiscard]] std::vector<float> sentence_embedding(
        std::span<const TokenId> tokens) const;

    void save(const std::filesystem::path& path) const;
    [[nodiscard]] static NeuralLanguageModel load(const std::filesystem::path& path);

private:
    struct GruLayer final {
        std::size_t input_size = 0U;
        Parameter update_input;
        Parameter update_recurrent;
        Parameter update_bias;
        Parameter reset_input;
        Parameter reset_recurrent;
        Parameter reset_bias;
        Parameter candidate_input;
        Parameter candidate_recurrent;
        Parameter candidate_bias;

        GruLayer(std::size_t index, std::size_t input, std::size_t hidden);
    };

    // Cache explícito guarda exatamente os valores necessários pela regra da cadeia.
    struct LayerCache final {
        std::vector<float> input;
        std::vector<float> previous_hidden;
        std::vector<float> update;
        std::vector<float> reset;
        std::vector<float> candidate;
        std::vector<float> gated_previous;
        std::vector<float> hidden;
    };

    struct StepCache final {
        TokenId input_token = 0U;
        TokenId target_token = 0U;
        std::vector<LayerCache> layers;
        std::vector<float> logits;
        std::vector<float> probabilities;
    };

    void validate_config() const;
    void validate_state(const State& state) const;
    void initialize_parameters();
    // cache é usado só no treino; output_logits pode ser nulo ao extrair embeddings.
    void forward_token(TokenId token,
                       State& state,
                       StepCache* cache,
                       std::vector<float>* output_logits) const;
    static void softmax(std::span<const float> logits, std::span<float> probabilities);
    static TokenId sample_logits(std::span<const float> logits,
                                 std::span<const std::size_t> recent_counts,
                                 const GenerationOptions& options,
                                 Random& random);

    void backward_segment(std::vector<StepCache>& steps);
    void apply_adam(std::size_t sample_count, const TrainingOptions& options);
    void zero_gradients();
    [[nodiscard]] std::vector<Parameter*> mutable_parameters();
    [[nodiscard]] std::vector<const Parameter*> parameters() const;

    ModelConfig config_;
    Parameter embedding_;
    std::vector<GruLayer> layers_;
    Parameter output_weight_;
    Parameter output_bias_;
    std::uint64_t optimizer_step_ = 0U;
};

} // namespace cortex
