#include "cortex/neural_model.hpp"

#include "cortex/tokenizer.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numeric>

namespace cortex {
namespace {

constexpr std::string_view model_magic = "CORTEX-GRU-1\n";
constexpr std::size_t maximum_vocabulary = 1'000'000U;
constexpr std::size_t maximum_width = 4'096U;
constexpr std::size_t maximum_layers = 16U;
constexpr long double maximum_parameters = 1'000'000'000.0L;

[[nodiscard]] ModelConfig checked_config(ModelConfig config) {
    if (config.vocabulary_size < BpeTokenizer::first_merge_id
        || config.vocabulary_size > maximum_vocabulary) {
        throw Error("vocabulary_size fora do intervalo seguro");
    }
    if (config.embedding_size == 0U || config.embedding_size > maximum_width) {
        throw Error("embedding_size fora do intervalo seguro");
    }
    if (config.hidden_size == 0U || config.hidden_size > maximum_width) {
        throw Error("hidden_size fora do intervalo seguro");
    }
    if (config.layer_count == 0U || config.layer_count > maximum_layers) {
        throw Error("layer_count fora do intervalo seguro");
    }

    const long double vocab = static_cast<long double>(config.vocabulary_size);
    const long double embedding = static_cast<long double>(config.embedding_size);
    const long double hidden = static_cast<long double>(config.hidden_size);
    const long double layers = static_cast<long double>(config.layer_count);
    const long double rough_count = vocab * embedding + vocab * hidden + vocab
        + 3.0L * hidden * (embedding + hidden + 1.0L)
        + std::max(0.0L, layers - 1.0L) * 3.0L * hidden * (2.0L * hidden + 1.0L);
    if (rough_count > maximum_parameters) {
        throw Error("configuração excede o limite de um bilhão de parâmetros");
    }
    return config;
}

void add_vector(std::span<float> destination, std::span<const float> source) {
    if (destination.size() != source.size()) {
        throw Error("vetores incompatíveis em add_vector");
    }
    for (std::size_t index = 0U; index < destination.size(); ++index) {
        destination[index] += source[index];
    }
}

void add_bias_gradient(Parameter& bias, std::span<const float> gradient) {
    if (bias.size() != gradient.size()) {
        throw Error("bias incompatível durante retropropagação");
    }
    add_vector(bias.gradient, gradient);
}

} // namespace

NeuralLanguageModel::GruLayer::GruLayer(std::size_t index,
                                        std::size_t input,
                                        std::size_t hidden)
    : input_size(input),
      update_input("layer." + std::to_string(index) + ".update_input", hidden, input),
      update_recurrent("layer." + std::to_string(index) + ".update_recurrent", hidden, hidden),
      update_bias("layer." + std::to_string(index) + ".update_bias", 1U, hidden),
      reset_input("layer." + std::to_string(index) + ".reset_input", hidden, input),
      reset_recurrent("layer." + std::to_string(index) + ".reset_recurrent", hidden, hidden),
      reset_bias("layer." + std::to_string(index) + ".reset_bias", 1U, hidden),
      candidate_input("layer." + std::to_string(index) + ".candidate_input", hidden, input),
      candidate_recurrent("layer." + std::to_string(index) + ".candidate_recurrent", hidden, hidden),
      candidate_bias("layer." + std::to_string(index) + ".candidate_bias", 1U, hidden) {}

NeuralLanguageModel::NeuralLanguageModel(ModelConfig config)
    : config_(checked_config(config)),
      embedding_("embedding", config_.vocabulary_size, config_.embedding_size),
      output_weight_("output.weight", config_.vocabulary_size, config_.hidden_size),
      output_bias_("output.bias", 1U, config_.vocabulary_size) {
    layers_.reserve(config_.layer_count);
    for (std::size_t index = 0U; index < config_.layer_count; ++index) {
        const std::size_t input = index == 0U ? config_.embedding_size : config_.hidden_size;
        layers_.emplace_back(index, input, config_.hidden_size);
    }
    initialize_parameters();
}

void NeuralLanguageModel::validate_config() const {
    static_cast<void>(checked_config(config_));
}

void NeuralLanguageModel::initialize_parameters() {
    Random random(config_.seed);
    // A tabela de embeddings usa escala ligada à dimensão, não ao vocabulário.
    embedding_.value.initialize_xavier(random, config_.embedding_size, config_.embedding_size);

    for (GruLayer& layer : layers_) {
        layer.update_input.value.initialize_xavier(random, layer.input_size, config_.hidden_size);
        layer.reset_input.value.initialize_xavier(random, layer.input_size, config_.hidden_size);
        layer.candidate_input.value.initialize_xavier(random, layer.input_size, config_.hidden_size);
        layer.update_recurrent.value.initialize_xavier(random, config_.hidden_size,
                                                        config_.hidden_size);
        layer.reset_recurrent.value.initialize_xavier(random, config_.hidden_size,
                                                       config_.hidden_size);
        layer.candidate_recurrent.value.initialize_xavier(random, config_.hidden_size,
                                                           config_.hidden_size);
        // Bias zero não favorece reset nem retenção antes de observar dados.
        layer.update_bias.value.fill(0.0F);
        layer.reset_bias.value.fill(0.0F);
        layer.candidate_bias.value.fill(0.0F);
    }
    output_weight_.value.initialize_xavier(random, config_.hidden_size,
                                           config_.vocabulary_size);
    output_bias_.value.fill(0.0F);
}

std::vector<Parameter*> NeuralLanguageModel::mutable_parameters() {
    std::vector<Parameter*> result;
    result.reserve(2U + layers_.size() * 9U);
    result.push_back(&embedding_);
    for (GruLayer& layer : layers_) {
        result.push_back(&layer.update_input);
        result.push_back(&layer.update_recurrent);
        result.push_back(&layer.update_bias);
        result.push_back(&layer.reset_input);
        result.push_back(&layer.reset_recurrent);
        result.push_back(&layer.reset_bias);
        result.push_back(&layer.candidate_input);
        result.push_back(&layer.candidate_recurrent);
        result.push_back(&layer.candidate_bias);
    }
    result.push_back(&output_weight_);
    result.push_back(&output_bias_);
    return result;
}

std::vector<const Parameter*> NeuralLanguageModel::parameters() const {
    std::vector<const Parameter*> result;
    result.reserve(2U + layers_.size() * 9U);
    result.push_back(&embedding_);
    for (const GruLayer& layer : layers_) {
        result.push_back(&layer.update_input);
        result.push_back(&layer.update_recurrent);
        result.push_back(&layer.update_bias);
        result.push_back(&layer.reset_input);
        result.push_back(&layer.reset_recurrent);
        result.push_back(&layer.reset_bias);
        result.push_back(&layer.candidate_input);
        result.push_back(&layer.candidate_recurrent);
        result.push_back(&layer.candidate_bias);
    }
    result.push_back(&output_weight_);
    result.push_back(&output_bias_);
    return result;
}

std::size_t NeuralLanguageModel::parameter_count() const noexcept {
    std::size_t count = embedding_.size() + output_weight_.size() + output_bias_.size();
    for (const GruLayer& layer : layers_) {
        count += layer.update_input.size() + layer.update_recurrent.size()
                 + layer.update_bias.size();
        count += layer.reset_input.size() + layer.reset_recurrent.size()
                 + layer.reset_bias.size();
        count += layer.candidate_input.size() + layer.candidate_recurrent.size()
                 + layer.candidate_bias.size();
    }
    return count;
}

NeuralLanguageModel::State NeuralLanguageModel::zero_state() const {
    return State(config_.layer_count, std::vector<float>(config_.hidden_size, 0.0F));
}

void NeuralLanguageModel::validate_state(const State& state) const {
    if (state.size() != config_.layer_count) {
        throw Error("estado recorrente possui quantidade incorreta de camadas");
    }
    for (const std::vector<float>& layer_state : state) {
        if (layer_state.size() != config_.hidden_size) {
            throw Error("estado recorrente possui largura incorreta");
        }
    }
}

void NeuralLanguageModel::forward_token(TokenId token,
                                         State& state,
                                         StepCache* cache,
                                         std::vector<float>* output_logits) const {
    if (static_cast<std::size_t>(token) >= config_.vocabulary_size) {
        throw Error("token fora do vocabulário do modelo");
    }
    if (cache != nullptr && output_logits != nullptr) {
        throw Error("forward_token recebeu dois destinos de logits");
    }
    validate_state(state);

    // A entrada é um span da tabela: inferência não copia a linha de embedding.
    std::span<const float> current_input = embedding_.value.row(token);
    if (cache != nullptr) {
        cache->input_token = token;
        cache->layers.clear();
        cache->layers.reserve(config_.layer_count);
    }

    for (std::size_t layer_index = 0U; layer_index < layers_.size(); ++layer_index) {
        const GruLayer& layer = layers_[layer_index];
        const std::vector<float>& previous = state[layer_index];
        std::vector<float> update(config_.hidden_size, 0.0F);
        std::vector<float> reset(config_.hidden_size, 0.0F);

        // z = sigmoid(Wz*x + Uz*h_anterior + bz)
        layer.update_input.value.matvec(current_input, update);
        layer.update_recurrent.value.matvec(previous, update, true);
        // r = sigmoid(Wr*x + Ur*h_anterior + br)
        layer.reset_input.value.matvec(current_input, reset);
        layer.reset_recurrent.value.matvec(previous, reset, true);
        for (std::size_t unit = 0U; unit < config_.hidden_size; ++unit) {
            update[unit] = stable_sigmoid(update[unit] + layer.update_bias.value.values()[unit]);
            reset[unit] = stable_sigmoid(reset[unit] + layer.reset_bias.value.values()[unit]);
        }

        // O estado anterior é filtrado antes de entrar no candidato.
        std::vector<float> gated_previous(config_.hidden_size, 0.0F);
        for (std::size_t unit = 0U; unit < config_.hidden_size; ++unit) {
            gated_previous[unit] = reset[unit] * previous[unit];
        }

        // n = tanh(Wn*x + Un*(r ⊙ h_anterior) + bn)
        std::vector<float> candidate(config_.hidden_size, 0.0F);
        layer.candidate_input.value.matvec(current_input, candidate);
        layer.candidate_recurrent.value.matvec(gated_previous, candidate, true);
        for (std::size_t unit = 0U; unit < config_.hidden_size; ++unit) {
            candidate[unit] = std::tanh(candidate[unit]
                                        + layer.candidate_bias.value.values()[unit]);
        }

        // h = (1-z) ⊙ n + z ⊙ h_anterior
        std::vector<float> hidden(config_.hidden_size, 0.0F);
        for (std::size_t unit = 0U; unit < config_.hidden_size; ++unit) {
            hidden[unit] = (1.0F - update[unit]) * candidate[unit]
                           + update[unit] * previous[unit];
        }

        if (cache != nullptr) {
            cache->layers.push_back(LayerCache{
                std::vector<float>(current_input.begin(), current_input.end()),
                previous,
                update,
                reset,
                candidate,
                gated_previous,
                hidden
            });
        }
        state[layer_index] = std::move(hidden);
        // A próxima camada lê diretamente o estado recém-produzido, sem cópia vertical.
        current_input = state[layer_index];
    }

    std::vector<float>* logits = cache != nullptr ? &cache->logits : output_logits;
    if (logits != nullptr) {
        logits->assign(config_.vocabulary_size, 0.0F);
        output_weight_.value.matvec(current_input, *logits);
        for (std::size_t token_index = 0U; token_index < config_.vocabulary_size; ++token_index) {
            (*logits)[token_index] += output_bias_.value.values()[token_index];
        }
    }
}

std::vector<float> NeuralLanguageModel::step(TokenId token, State& state) const {
    std::vector<float> logits;
    // Inferência não materializa os sete vetores de cache usados exclusivamente pelo BPTT.
    forward_token(token, state, nullptr, &logits);
    return logits;
}

void NeuralLanguageModel::softmax(std::span<const float> logits,
                                  std::span<float> probabilities) {
    if (logits.empty() || probabilities.size() != logits.size()) {
        throw Error("dimensões inválidas em softmax");
    }
    const float maximum = *std::max_element(logits.begin(), logits.end());
    double total = 0.0;
    for (std::size_t index = 0U; index < logits.size(); ++index) {
        const float exponential = std::exp(logits[index] - maximum);
        probabilities[index] = exponential;
        total += static_cast<double>(exponential);
    }
    if (!(total > 0.0) || !std::isfinite(total)) {
        throw Error("softmax encontrou logits não finitos");
    }
    const float inverse = static_cast<float>(1.0 / total);
    for (float& probability : probabilities) {
        probability *= inverse;
    }
}

void NeuralLanguageModel::zero_gradients() {
    for (Parameter* parameter : mutable_parameters()) {
        parameter->zero_gradient();
    }
}

void NeuralLanguageModel::backward_segment(std::vector<StepCache>& steps) {
    if (steps.empty()) {
        return;
    }

    // future_gradient[l] é dL/dh_l vindo do próximo instante temporal.
    std::vector<std::vector<float>> future_gradient(
        config_.layer_count, std::vector<float>(config_.hidden_size, 0.0F));

    for (std::size_t reverse_time = steps.size(); reverse_time-- > 0U;) {
        StepCache& step_cache = steps[reverse_time];
        std::vector<float> logit_gradient = step_cache.probabilities;
        logit_gradient[step_cache.target_token] -= 1.0F; // softmax + cross-entropy

        const std::vector<float>& top_hidden = step_cache.layers.back().hidden;
        add_outer_product(output_weight_, logit_gradient, top_hidden);
        add_bias_gradient(output_bias_, logit_gradient);

        // Gradiente da projeção de saída em direção à última camada recorrente.
        std::vector<float> gradient_from_upper(config_.hidden_size, 0.0F);
        output_weight_.value.transposed_matvec(logit_gradient, gradient_from_upper);

        for (std::size_t reverse_layer = layers_.size(); reverse_layer-- > 0U;) {
            GruLayer& layer = layers_[reverse_layer];
            const LayerCache& values = step_cache.layers[reverse_layer];
            std::vector<float> hidden_gradient = gradient_from_upper;
            add_vector(hidden_gradient, future_gradient[reverse_layer]);

            std::vector<float> candidate_pre_gradient(config_.hidden_size, 0.0F);
            std::vector<float> update_pre_gradient(config_.hidden_size, 0.0F);
            std::vector<float> previous_gradient(config_.hidden_size, 0.0F);

            // Regra da cadeia através de h=(1-z)n+z*h_prev e das ativações.
            for (std::size_t unit = 0U; unit < config_.hidden_size; ++unit) {
                const float candidate_gradient = hidden_gradient[unit]
                                                 * (1.0F - values.update[unit]);
                const float update_gradient = hidden_gradient[unit]
                                              * (values.previous_hidden[unit]
                                                 - values.candidate[unit]);
                previous_gradient[unit] = hidden_gradient[unit] * values.update[unit];
                candidate_pre_gradient[unit] = candidate_gradient
                                               * (1.0F - values.candidate[unit]
                                                           * values.candidate[unit]);
                update_pre_gradient[unit] = update_gradient * values.update[unit]
                                            * (1.0F - values.update[unit]);
            }

            add_outer_product(layer.candidate_input, candidate_pre_gradient, values.input);
            add_outer_product(layer.candidate_recurrent, candidate_pre_gradient,
                              values.gated_previous);
            add_bias_gradient(layer.candidate_bias, candidate_pre_gradient);

            std::vector<float> input_gradient(layer.input_size, 0.0F);
            layer.candidate_input.value.transposed_matvec(candidate_pre_gradient,
                                                          input_gradient);
            std::vector<float> gated_previous_gradient(config_.hidden_size, 0.0F);
            layer.candidate_recurrent.value.transposed_matvec(candidate_pre_gradient,
                                                              gated_previous_gradient);

            std::vector<float> reset_pre_gradient(config_.hidden_size, 0.0F);
            for (std::size_t unit = 0U; unit < config_.hidden_size; ++unit) {
                const float reset_gradient = gated_previous_gradient[unit]
                                             * values.previous_hidden[unit];
                reset_pre_gradient[unit] = reset_gradient * values.reset[unit]
                                           * (1.0F - values.reset[unit]);
                previous_gradient[unit] += gated_previous_gradient[unit]
                                           * values.reset[unit];
            }

            add_outer_product(layer.reset_input, reset_pre_gradient, values.input);
            add_outer_product(layer.reset_recurrent, reset_pre_gradient,
                              values.previous_hidden);
            add_bias_gradient(layer.reset_bias, reset_pre_gradient);

            std::vector<float> reset_input_gradient(layer.input_size, 0.0F);
            layer.reset_input.value.transposed_matvec(reset_pre_gradient,
                                                      reset_input_gradient);
            add_vector(input_gradient, reset_input_gradient);
            std::vector<float> reset_previous_gradient(config_.hidden_size, 0.0F);
            layer.reset_recurrent.value.transposed_matvec(reset_pre_gradient,
                                                          reset_previous_gradient);
            add_vector(previous_gradient, reset_previous_gradient);

            add_outer_product(layer.update_input, update_pre_gradient, values.input);
            add_outer_product(layer.update_recurrent, update_pre_gradient,
                              values.previous_hidden);
            add_bias_gradient(layer.update_bias, update_pre_gradient);

            std::vector<float> update_input_gradient(layer.input_size, 0.0F);
            layer.update_input.value.transposed_matvec(update_pre_gradient,
                                                       update_input_gradient);
            add_vector(input_gradient, update_input_gradient);
            std::vector<float> update_previous_gradient(config_.hidden_size, 0.0F);
            layer.update_recurrent.value.transposed_matvec(update_pre_gradient,
                                                           update_previous_gradient);
            add_vector(previous_gradient, update_previous_gradient);

            // O gradiente temporal segue para t-1; o vertical segue para a camada inferior.
            future_gradient[reverse_layer] = std::move(previous_gradient);
            gradient_from_upper = std::move(input_gradient);
        }

        // A camada inferior recebe como entrada a linha da tabela de embeddings.
        const std::size_t embedding_offset = static_cast<std::size_t>(step_cache.input_token)
                                             * config_.embedding_size;
        for (std::size_t dimension = 0U; dimension < config_.embedding_size; ++dimension) {
            embedding_.gradient[embedding_offset + dimension] += gradient_from_upper[dimension];
        }
    }
}

void NeuralLanguageModel::apply_adam(std::size_t sample_count,
                                     const TrainingOptions& options) {
    if (sample_count == 0U) {
        return;
    }
    const float inverse_samples = 1.0F / static_cast<float>(sample_count);
    long double squared_norm = 0.0L;
    for (const Parameter* parameter : parameters()) {
        for (const float raw_gradient : parameter->gradient) {
            const long double gradient = static_cast<long double>(raw_gradient * inverse_samples);
            squared_norm += gradient * gradient;
        }
    }

    const float norm = static_cast<float>(std::sqrt(squared_norm));
    const float clip_scale = norm > options.gradient_clip
                             ? options.gradient_clip / std::max(norm, 1.0e-12F)
                             : 1.0F;
    ++optimizer_step_;
    const float first_correction = 1.0F
        - std::pow(options.beta1, static_cast<float>(optimizer_step_));
    const float second_correction = 1.0F
        - std::pow(options.beta2, static_cast<float>(optimizer_step_));

    for (Parameter* parameter : mutable_parameters()) {
        std::vector<float>& weights = parameter->value.values();
        for (std::size_t index = 0U; index < parameter->size(); ++index) {
            const float gradient = parameter->gradient[index] * inverse_samples * clip_scale;
            parameter->first_moment[index] = options.beta1 * parameter->first_moment[index]
                                             + (1.0F - options.beta1) * gradient;
            parameter->second_moment[index] = options.beta2 * parameter->second_moment[index]
                                              + (1.0F - options.beta2)
                                                    * gradient * gradient;
            const float corrected_first = parameter->first_moment[index] / first_correction;
            const float corrected_second = parameter->second_moment[index] / second_correction;
            weights[index] -= options.learning_rate * corrected_first
                              / (std::sqrt(corrected_second) + options.epsilon);
        }
        parameter->zero_gradient();
    }
}

std::vector<TrainingMetrics> NeuralLanguageModel::train(
    std::span<const TokenId> tokens,
    const TrainingOptions& options,
    const std::function<void(const TrainingMetrics&)>& on_epoch) {
    if (tokens.size() < 2U) {
        throw Error("treinamento exige pelo menos dois tokens");
    }
    if (options.epochs == 0U || options.sequence_length == 0U) {
        throw Error("epochs e sequence_length devem ser positivos");
    }
    if (!(options.learning_rate > 0.0F) || !std::isfinite(options.learning_rate)
        || !(options.gradient_clip > 0.0F) || !std::isfinite(options.gradient_clip)
        || !(options.beta1 > 0.0F && options.beta1 < 1.0F)
        || !(options.beta2 > 0.0F && options.beta2 < 1.0F)
        || !(options.epsilon > 0.0F)) {
        throw Error("hiperparâmetros de treinamento inválidos");
    }
    for (const TokenId token : tokens) {
        if (static_cast<std::size_t>(token) >= config_.vocabulary_size) {
            throw Error("sequência de treino contém token fora do vocabulário");
        }
    }

    std::vector<TrainingMetrics> history;
    history.reserve(options.epochs);
    zero_gradients();

    for (std::size_t epoch = 1U; epoch <= options.epochs; ++epoch) {
        State state = zero_state();
        double accumulated_loss = 0.0;
        std::size_t predicted = 0U;

        for (std::size_t offset = 0U; offset + 1U < tokens.size();) {
            const std::size_t count = std::min(options.sequence_length,
                                               tokens.size() - offset - 1U);
            std::vector<StepCache> steps;
            steps.reserve(count);

            for (std::size_t local = 0U; local < count; ++local) {
                StepCache cache;
                cache.target_token = tokens[offset + local + 1U];
                forward_token(tokens[offset + local], state, &cache, nullptr);
                cache.probabilities.assign(config_.vocabulary_size, 0.0F);
                softmax(cache.logits, cache.probabilities);
                const float target_probability = std::max(
                    cache.probabilities[cache.target_token], 1.0e-20F);
                accumulated_loss -= std::log(static_cast<double>(target_probability));
                steps.push_back(std::move(cache));
            }

            zero_gradients();
            backward_segment(steps);
            apply_adam(count, options);
            predicted += count;
            offset += count;
        }

        const float average = static_cast<float>(accumulated_loss
                                                  / static_cast<double>(predicted));
        const TrainingMetrics metrics{
            epoch,
            average,
            std::exp(std::min(average, 20.0F)),
            predicted
        };
        history.push_back(metrics);
        if (on_epoch) {
            on_epoch(metrics);
        }
    }
    return history;
}

float NeuralLanguageModel::evaluate_loss(std::span<const TokenId> tokens) const {
    if (tokens.size() < 2U) {
        throw Error("avaliação exige pelo menos dois tokens");
    }
    State state = zero_state();
    double loss = 0.0;
    for (std::size_t index = 0U; index + 1U < tokens.size(); ++index) {
        const TokenId target = tokens[index + 1U];
        if (static_cast<std::size_t>(target) >= config_.vocabulary_size) {
            throw Error("avaliação contém token fora do vocabulário");
        }
        const std::vector<float> logits = step(tokens[index], state);
        std::vector<float> probabilities(config_.vocabulary_size, 0.0F);
        softmax(logits, probabilities);
        loss -= std::log(static_cast<double>(std::max(probabilities[target], 1.0e-20F)));
    }
    return static_cast<float>(loss / static_cast<double>(tokens.size() - 1U));
}

TokenId NeuralLanguageModel::sample_logits(std::span<const float> logits,
                                            std::span<const std::size_t> recent_counts,
                                            const GenerationOptions& options,
                                            Random& random) {
    if (logits.size() != recent_counts.size() || logits.size() <= BpeTokenizer::eos_id) {
        throw Error("estado inválido para amostragem");
    }
    if (!(options.temperature > 0.0F) || !std::isfinite(options.temperature)
        || !(options.repetition_penalty >= 1.0F)
        || !std::isfinite(options.repetition_penalty)) {
        throw Error("parâmetros de geração inválidos");
    }

    // PAD e BOS não são resultados válidos; EOS permanece para encerrar naturalmente.
    std::vector<std::size_t> candidates(logits.size() - BpeTokenizer::eos_id);
    std::iota(candidates.begin(), candidates.end(), BpeTokenizer::eos_id);
    const auto adjusted = [&logits, &recent_counts, &options](std::size_t index) {
        float value = logits[index];
        if (recent_counts[index] != 0U) {
            value = value >= 0.0F ? value / options.repetition_penalty
                                  : value * options.repetition_penalty;
        }
        return value / options.temperature;
    };

    const std::size_t wanted = options.top_k == 0U
                               ? candidates.size()
                               : std::min(options.top_k, candidates.size());
    if (wanted < candidates.size()) {
        std::nth_element(candidates.begin(),
                         candidates.begin() + static_cast<std::ptrdiff_t>(wanted),
                         candidates.end(),
                         [&adjusted](std::size_t left, std::size_t right) {
                             return adjusted(left) > adjusted(right);
                         });
        candidates.resize(wanted);
    }

    float maximum = -std::numeric_limits<float>::infinity();
    for (const std::size_t candidate : candidates) {
        maximum = std::max(maximum, adjusted(candidate));
    }
    std::vector<float> weights(candidates.size(), 0.0F);
    double total = 0.0;
    for (std::size_t index = 0U; index < candidates.size(); ++index) {
        weights[index] = std::exp(adjusted(candidates[index]) - maximum);
        total += static_cast<double>(weights[index]);
    }

    double threshold = static_cast<double>(random.uniform()) * total;
    for (std::size_t index = 0U; index < candidates.size(); ++index) {
        threshold -= static_cast<double>(weights[index]);
        if (threshold <= 0.0) {
            return static_cast<TokenId>(candidates[index]);
        }
    }
    return static_cast<TokenId>(candidates.back());
}

std::vector<TokenId> NeuralLanguageModel::generate(
    std::span<const TokenId> prompt,
    const GenerationOptions& options) const {
    State state = zero_state();
    std::vector<std::size_t> recent_counts(config_.vocabulary_size, 0U);
    std::vector<float> logits;

    if (prompt.empty()) {
        logits = step(BpeTokenizer::bos_id, state);
        ++recent_counts[BpeTokenizer::bos_id];
    } else {
        for (const TokenId token : prompt) {
            if (static_cast<std::size_t>(token) >= config_.vocabulary_size) {
                throw Error("prompt contém token fora do vocabulário");
            }
            logits = step(token, state);
            ++recent_counts[token];
        }
    }

    Random random(options.seed);
    std::vector<TokenId> generated;
    generated.reserve(options.maximum_tokens);
    for (std::size_t index = 0U; index < options.maximum_tokens; ++index) {
        const TokenId token = sample_logits(logits, recent_counts, options, random);
        if (token == BpeTokenizer::eos_id) {
            break;
        }
        generated.push_back(token);
        ++recent_counts[token];
        logits = step(token, state);
    }
    return generated;
}

std::vector<float> NeuralLanguageModel::sentence_embedding(
    std::span<const TokenId> tokens) const {
    std::vector<float> embedding(config_.hidden_size, 0.0F);
    if (tokens.empty()) {
        return embedding;
    }

    State state = zero_state();
    for (const TokenId token : tokens) {
        // Embeddings não precisam da projeção para o vocabulário, economizando V×H operações.
        forward_token(token, state, nullptr, nullptr);
        add_vector(embedding, state.back());
    }
    const float inverse_count = 1.0F / static_cast<float>(tokens.size());
    for (float& value : embedding) {
        value *= inverse_count;
    }
    normalize_in_place(embedding);
    return embedding;
}

void NeuralLanguageModel::save(const std::filesystem::path& path) const {
    const std::filesystem::path temporary = temporary_path_for(path);
    try {
        {
            BinaryWriter writer(temporary);
            writer.magic(model_magic);
            writer.u32(static_cast<std::uint32_t>(config_.vocabulary_size));
            writer.u32(static_cast<std::uint32_t>(config_.embedding_size));
            writer.u32(static_cast<std::uint32_t>(config_.hidden_size));
            writer.u32(static_cast<std::uint32_t>(config_.layer_count));
            writer.u64(config_.seed);
            writer.u64(optimizer_step_);

            const std::vector<const Parameter*> all = parameters();
            writer.u32(static_cast<std::uint32_t>(all.size()));
            for (const Parameter* parameter : all) {
                writer.string(parameter->name);
                writer.u32(static_cast<std::uint32_t>(parameter->value.rows()));
                writer.u32(static_cast<std::uint32_t>(parameter->value.columns()));
                writer.u64(static_cast<std::uint64_t>(parameter->size()));
                for (const float value : parameter->value.values()) {
                    writer.f32(value);
                }
                for (const float value : parameter->first_moment) {
                    writer.f32(value);
                }
                for (const float value : parameter->second_moment) {
                    writer.f32(value);
                }
            }
            writer.flush();
            writer.close();
        }
        replace_file(temporary, path);
    } catch (...) {
        std::filesystem::remove(temporary);
        throw;
    }
}

NeuralLanguageModel NeuralLanguageModel::load(const std::filesystem::path& path) {
    BinaryReader reader(path);
    reader.expect_magic(model_magic);
    ModelConfig config;
    config.vocabulary_size = static_cast<std::size_t>(reader.u32());
    config.embedding_size = static_cast<std::size_t>(reader.u32());
    config.hidden_size = static_cast<std::size_t>(reader.u32());
    config.layer_count = static_cast<std::size_t>(reader.u32());
    config.seed = reader.u64();

    NeuralLanguageModel model(config); // A validação ocorre antes de qualquer grande alocação.
    model.optimizer_step_ = reader.u64();
    std::vector<Parameter*> expected = model.mutable_parameters();
    const std::size_t count = static_cast<std::size_t>(reader.u32());
    if (count != expected.size()) {
        throw Error("checkpoint possui quantidade inesperada de parâmetros");
    }

    for (Parameter* parameter : expected) {
        const std::string name = reader.string(256U);
        const std::size_t rows = static_cast<std::size_t>(reader.u32());
        const std::size_t columns = static_cast<std::size_t>(reader.u32());
        const std::size_t size = static_cast<std::size_t>(reader.u64());
        if (name != parameter->name
            || rows != parameter->value.rows()
            || columns != parameter->value.columns()
            || size != parameter->size()) {
            throw Error("forma ou nome de parâmetro incompatível no checkpoint: " + name);
        }
        for (float& value : parameter->value.values()) {
            value = reader.f32();
        }
        for (float& value : parameter->first_moment) {
            value = reader.f32();
        }
        for (float& value : parameter->second_moment) {
            value = reader.f32();
        }
    }
    reader.require_end();
    model.validate_config();
    return model;
}

} // namespace cortex
