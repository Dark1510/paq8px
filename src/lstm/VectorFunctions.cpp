#include "VectorFunctions.hpp"
#include "Utils.hpp" // bitcast_u32_to_f32

#include <numeric>
#include <cassert>
#include <cstring>

// ============================================================================
// Padé Approximants for Activation Functions
// ============================================================================

// Tanh Padé approximant: tanh(x) ≈ x(27 + x²) / (27 + 9x²)
static float constexpr tanh_pade(float x)
{
  float x2 = x * x;
  return (x * (27.0f + x2)) / (27.0f + 9.0f * x2);
}

// Sigmoid via tanh: σ(x) = 0.5 * (1 + tanh(x/2))
static float constexpr sigmoid_pade(float x)
{
  float u = 0.5f * x;
  float u2 = u * u;
  float tanh_val = (u * (27.0f + u2)) / (27.0f + 9.0f * u2);
  return 0.5f * (1.0f + tanh_val);
}

static constexpr float SIGMOID_MIN = sigmoid_pade(SIGMOID_CLIP_MIN);
static constexpr float SIGMOID_MAX = sigmoid_pade(SIGMOID_CLIP_MAX);
static constexpr float TANH_MIN = tanh_pade(TANH_CLIP_MIN);
static constexpr float TANH_MAX = tanh_pade(TANH_CLIP_MAX);

static_assert(SIGMOID_MIN > 0.0f);
static_assert(SIGMOID_MAX < 1.0f);
static_assert(TANH_MIN > -1.0f);
static_assert(TANH_MAX < +1.0f);

float tanh_pade_clipped(float x)
{
  if (x > TANH_CLIP_MAX)
      return TANH_MAX;
  if (x < TANH_CLIP_MIN)
      return TANH_MIN;
  return tanh_pade(x);
}

// Sigmoid via tanh: σ(x) = 0.5 * (1 + tanh(x/2))
float sigmoid_pade_clipped(float x)
{
  if (x > SIGMOID_CLIP_MAX)
      return SIGMOID_MAX;
  if (x < SIGMOID_CLIP_MIN)
      return SIGMOID_MIN;
  return sigmoid_pade(x);
}

// VectorFunctions_Scalar


// Static helper functions

static float horizontal_sum(float x0, float x1, float x2, float x3, float x4, float x5, float x6, float x7)
{
  // Simulate loading of __m256 as an array of 8 floats
  float sum0 = x0 + x4; // Pair 0
  float sum1 = x1 + x5; // Pair 1
  float sum2 = x2 + x6; // Pair 2
  float sum3 = x3 + x7; // Pair 3

  // Combine pairs
  sum0 = sum0 + sum2;
  sum1 = sum1 + sum3;

  // Final horizontal sum
  sum0 = sum0 + sum1;

  return sum0;
}

// expf
// Cody–Waite style reduction with split ln2 (hi+lo) and degree-5 polynomial on the reduced range.
// Preconditions: x is finite; FP rounding mode is round-to-nearest-even. (softmax pipeline guarantees these)
// => no need to check isfinite - for any case we'll check outputs on the result later
// Notes:
//   Max relative error ≈ 3.37e-7
//   Max ULP error ≈ 4 ULP
//   Median ULP ≈ 1, mean ≈ 0.61
//   Monotonic, strictly positive over the whole domain.
// to get hex literals use: printf("%a\n", x);  // %a for hex float
// see also https://chromium.googlesource.com/external/github.com/google/XNNPACK/+/refs/heads/upstream/test_638074745/src/math/f32-sigmoid-sse2-rr2-p5-div.c

static inline float expf_compat(float x) {
  //keep (n + 127) in [1,254] for a valid exponent field
  if (x < -87)
    x = -87; //final result will be 1.64581131e-38

  // Cody–Waite split of ln2
  const float INV_LN2 = 0x1.715476p+0f;   // 1.4426950216f  // properly rounded 1/ln2
  const float LN2_HI = 0x1.62e400p-1f;    // 0.693145752f   // coarsened value of ln2 i.e. with zeroed 7 last fraction bits
  const float LN2_LO = 0x1.7f7d1cp-20f;   // 1.42860677e-6f // ln2-LN2_HI
  //This way LN2_HI + LN2_LO (in exact real arithmetic) is extremely close to ln2, and when computed in float it reproduces the correctly rounded float value of ln⁡2.
  //There are many valid Cody–Waite splits; this pair (hi = 0x1.62e400p-1f, lo = 0x1.7f7d1cp-20f) is a well-tested single-precision choice that balances reduction error for ∣n∣≲126.

  // n = round(x / ln2)
  // t = (float)n
  float z = x * INV_LN2;
  const float MAGIC_BIAS = 12582912;   // 1.5 x 2^23 : trick for rounding
  float t = z + MAGIC_BIAS;
  int   n = (int)t - 12582912;         // subtract the bias in integer domain;
  t -= MAGIC_BIAS;

  // r = x - n*ln2 using split constants (Cody–Waite)
  float r = x - t * LN2_HI;
  r = r - t * LN2_LO;

  // Taylor coefficients
  // exp(r) ≈ 1 + r + c2 r^2 + c3 r^3 + c4 r^4 + c5 r^5
  const float c2 = 0x1.fffe24p-2f;  // ≈ 0.499992907 (≈ 1/2)
  const float c3 = 0x1.5554acp-3f;  // ≈ 0.166665405 (≈ 1/6)
  const float c4 = 0x1.5713a4p-5f;  // ≈ 0.041879482 (≈ 1/24)
  const float c5 = 0x1.12266ap-7f;  // ≈ 0.008366401 (≈ 1/120)

  // Estrin's Scheme
  // It gives shorter dependency chains than Horner, which usually wins on SIMD and GPUs.
  float r2 = r * r;
  float q2 = c2 + c3 * r;
  float q4 = c4 + c5 * r;
  float p = (q4 * r2 + q2) * r2 + (r + 1.0f);

  // construct 2^n as float via exponent bits: (n + 127) << 23
  uint32_t expbits = (uint32_t)(n + 127);
  // note: expbits must be in [1,254] for normalized; we've clamped x earlier.
  uint32_t bits = expbits << 23;
  float two_n = bitcast_u32_to_f32(bits);

  return p * two_n;
}

// Member implementations

void VectorFunctions_Scalar::Copy(float* dst, const float* src, size_t num_floats) {
  memcpy(dst, src, num_floats * sizeof(float));
}

void VectorFunctions_Scalar::Zero(float* dst, size_t num_floats) {
  memset(dst, 0, num_floats * sizeof(float));
}

float VectorFunctions_Scalar::DotProduct(
  float const* x1,
  float const* x2,
  size_t const len)
{
  float sum0 = 0.0f, sum1 = 0.0f, sum2 = 0.0f, sum3 = 0.0f;
  float sum4 = 0.0f, sum5 = 0.0f, sum6 = 0.0f, sum7 = 0.0f;

  for (size_t i = 0; i < len; i += 8)
  {
    sum0 += x1[i + 0] * x2[i + 0];
    sum1 += x1[i + 1] * x2[i + 1];
    sum2 += x1[i + 2] * x2[i + 2];
    sum3 += x1[i + 3] * x2[i + 3];
    sum4 += x1[i + 4] * x2[i + 4];
    sum5 += x1[i + 5] * x2[i + 5];
    sum6 += x1[i + 6] * x2[i + 6];
    sum7 += x1[i + 7] * x2[i + 7];
  }

  return horizontal_sum(sum0, sum1, sum2, sum3, sum4, sum5, sum6, sum7);
}


float VectorFunctions_Scalar::SumOfSquares(float* array, size_t array_length)
{
  float sum0 = 0.0f, sum1 = 0.0f, sum2 = 0.0f, sum3 = 0.0f;
  float sum4 = 0.0f, sum5 = 0.0f, sum6 = 0.0f, sum7 = 0.0f;

  for (size_t i = 0; i < array_length; i += 8)
  {
    float x0 = array[i];
    float x1 = array[i + 1];
    float x2 = array[i + 2];
    float x3 = array[i + 3];
    float x4 = array[i + 4];
    float x5 = array[i + 5];
    float x6 = array[i + 6];
    float x7 = array[i + 7];

    sum0 += x0 * x0;
    sum1 += x1 * x1;
    sum2 += x2 * x2;
    sum3 += x3 * x3;
    sum4 += x4 * x4;
    sum5 += x5 * x5;
    sum6 += x6 * x6;
    sum7 += x7 * x7;
  }
  return horizontal_sum(sum0, sum1, sum2, sum3, sum4, sum5, sum6, sum7);
}

void VectorFunctions_Scalar::NormalizeThenActivate_Sigmoid(
  size_t array_length,
  float* to_be_normalized_values,
  float* activations_out,
  float* gamma,
  float* beta,
  float rms_scale)
{
  for (size_t i = 0; i < array_length; i++) {
    float n = to_be_normalized_values[i] * rms_scale;
    to_be_normalized_values[i] = n;
    activations_out[i] = sigmoid_pade_clipped(n * gamma[i] + beta[i]);
  }
}

void VectorFunctions_Scalar::NormalizeThenActivate_Tanh(
  size_t array_length,
  float* to_be_normalized_values,
  float* activations_out,
  float* gamma,
  float* beta,
  float rms_scale)
{
  for (size_t i = 0; i < array_length; i++) {
    float n = to_be_normalized_values[i] * rms_scale;
    to_be_normalized_values[i] = n;
    activations_out[i] = tanh_pade_clipped(n * gamma[i] + beta[i]);
  }
}

void VectorFunctions_Scalar::AccumulateLstmGradients(
  size_t hidden_size,
  size_t concatenated_hidden_size,
  size_t vocabulary_size,
  size_t layer_id,
  float* error_on_output,
  float* hidden_gradient_accumulator,
  float* output_weights)
{
  size_t output_layer_offset = layer_id * hidden_size; // layer_id * 200
  for (size_t i = 0; i < vocabulary_size; i++) {   // 256 iterations
    float const error = error_on_output[i];
    for (size_t j = 0; j < hidden_size; j++) { // 200 iterations
      hidden_gradient_accumulator[j] += output_weights[output_layer_offset + j] * error;
    }
    output_layer_offset += concatenated_hidden_size;
  }
}

void VectorFunctions_Scalar::AccumulateLstmLayerGradients(
  size_t hidden_size,
  size_t timestep_offset,
  float* gradient_from_next_timestep,
  float* hidden_gradient_accumulator,
  float* tanh_state,
  float* forget_gate_activations,
  float* cell_candidate_activations,
  float* output_gate_activations,
  float* output_gate_gradients,
  float* cell_state_gradient,
  float* cell_candidate_gradients,
  float* forget_gate_gradients,
  float* last_cell_state)
{
  for (size_t i = 0; i < hidden_size; i++) {          // 200 iterations
    gradient_from_next_timestep[i] += hidden_gradient_accumulator[i];
    hidden_gradient_accumulator[i] = 0.0f;

    const size_t idx = timestep_offset + i;         // sequence_position*200 + i
    const float tanh_v = tanh_state[idx];
    const float forget_gate = forget_gate_activations[idx];
    const float cell_candidate = cell_candidate_activations[idx];
    const float output_gate = output_gate_activations[idx];
    const float input_gate = 1.0f - forget_gate;

    output_gate_gradients[i] =
      tanh_v * gradient_from_next_timestep[i] *
      output_gate * (1.0f - output_gate); // sigmoid derivative: σ'(x) = σ(x) × (1 - σ(x))

    cell_state_gradient[i] +=
      gradient_from_next_timestep[i] * output_gate *
      (1.0f - tanh_v * tanh_v); // tanh derivative: tanh'(x) = 1 - tanh²(x)

    cell_candidate_gradients[i] =
      cell_state_gradient[i] * input_gate *
      (1.0f - cell_candidate * cell_candidate); // tanh derivative: tanh'(x) = 1 - tanh²(x)

    forget_gate_gradients[i] =
      (last_cell_state[idx] - cell_candidate) *
      cell_state_gradient[i] *
      forget_gate * input_gate; // implicit sigmoid derivative: forget * input_gate where input_gate = 1.0f - forget

    if (timestep_offset > 0) { // sequence_position > 0
      cell_state_gradient[i] *= forget_gate;
      gradient_from_next_timestep[i] = 0.0f;
    }
  }
}

void VectorFunctions_Scalar::BackpropagateErrors(
  size_t len,                       // hidden_size (200)
  size_t base_offset,               // 0 for temporal, hidden_size for spatial
  size_t component_input_dim,       // Layer 0: 200, Layer 1: 400
  float* weights,                   // Weight matrix
  float* pre_activation_gradients,  // Current layer errors
  float* grad_store)                // Where to accumulate gradients
{
  for (size_t i = 0; i < len; i += 8) { // For each cell in previous layer's hidden state
    // for better precision calculate the sum then add to the existing grads
    float sum0 = 0;
    float sum1 = 0;
    float sum2 = 0;
    float sum3 = 0;
    float sum4 = 0;
    float sum5 = 0;
    float sum6 = 0;
    float sum7 = 0;

    size_t weight_idx = base_offset + i;  // Start at offset for previous layer connections
    for (size_t i = 0; i < len; i++) { // For each current cell
      float g = pre_activation_gradients[i];
      sum0 += g * weights[weight_idx + 0];
      sum1 += g * weights[weight_idx + 1];
      sum2 += g * weights[weight_idx + 2];
      sum3 += g * weights[weight_idx + 3];
      sum4 += g * weights[weight_idx + 4];
      sum5 += g * weights[weight_idx + 5];
      sum6 += g * weights[weight_idx + 6];
      sum7 += g * weights[weight_idx + 7];
      weight_idx += component_input_dim;  // Move to next cell's weights
    }

    grad_store[i + 0] += sum0;
    grad_store[i + 1] += sum1;
    grad_store[i + 2] += sum2;
    grad_store[i + 3] += sum3;
    grad_store[i + 4] += sum4;
    grad_store[i + 5] += sum5;
    grad_store[i + 6] += sum6;
    grad_store[i + 7] += sum7;
  }
}

void VectorFunctions_Scalar::AccumulateLayerGradients(
  const size_t hidden_size,
  const size_t vocabulary_size,
  const size_t component_input_dim,
  const float* input,
  const float* pre_activation_gradients,
  float* embedding_ptr,
  float* weight_gradients)
{
  for (size_t i = 0; i < hidden_size; i++) {
    const float g = pre_activation_gradients[i];

    // Update symbol_embeddings gradient
    *embedding_ptr += g;
    embedding_ptr += vocabulary_size;

    // Update hidden state weight gradients
    for (size_t j = 0; j < component_input_dim; j++)
      weight_gradients[j] += g * input[j];

    weight_gradients += component_input_dim;
  }
}

void VectorFunctions_Scalar::AccumulateOutputLayerGradients(
  size_t previous_output_offset,
  float* error_on_output,
  float* output_weight_gradients,
  float* output_bias_gradients,
  const float* hidden_ptr,
  const size_t vocabulary_size,
  const size_t concatenated_hidden_size,
  const size_t input_symbol)
{

  for (size_t i = 0; i < vocabulary_size; i++) {
    float error = error_on_output[i];
    output_bias_gradients[i] += error;

    for (size_t j = 0; j < concatenated_hidden_size; j++) {
      output_weight_gradients[j] += error * hidden_ptr[j];
    }

    output_weight_gradients += concatenated_hidden_size;
  }
}

float VectorFunctions_Scalar::ComputeMaxLogit(
  float* result,
  size_t result_length)
{
  float maxlogit = negative_infinity;
  for (size_t i = 0; i < result_length; i++) {
    if (result[i] > maxlogit)
      maxlogit = result[i];
  }
  return maxlogit;
}

void VectorFunctions_Scalar::MatvecThenSoftmax(
  float* hidden,
  float* logits,
  float* output_weights,
  float* output,
  float* output_bias,
  size_t const concatenated_hidden_size, // 200*2 = 400
  size_t const vocabulary_size, // 256
  size_t const output_offset
)
{
  // Compute logits via dot products
  for (size_t i = 0; i < vocabulary_size; i++) {   // 256 iterations
    logits[output_offset + i] = DotProduct( // logits[sequence_position * 256 + i]
      &hidden[0],
      &output_weights[i * concatenated_hidden_size],
      concatenated_hidden_size // 400
    ) + output_bias[i];
  }

  // Find max logit for numerical stability
  float max_logit = ComputeMaxLogit(&logits[output_offset], vocabulary_size);

  // Compute softmax
  Softmax(
    &logits[output_offset],                  // &logits[sequence_position * 256]
    &output[output_offset],                  // &output[sequence_position * 256]
    vocabulary_size,                             // 256
    max_logit);
}

void VectorFunctions_Scalar::Softmax(
  float* logits,
  float* probs,
  size_t len,
  float max_logit)
{
  float expsum[8]{ 0.0f };
  for (size_t i = 0; i < len; i += 8) {
    for (size_t j = 0; j < 8; j++) {
      float x = expf_compat(logits[i + j] - max_logit);
      probs[i + j] = x;
      expsum[j] += x;
    }
  }
  float expsum_reciprocal = 1.0f / horizontal_sum(expsum[0], expsum[1], expsum[2], expsum[3], expsum[4], expsum[5], expsum[6], expsum[7]);
  for (size_t i = 0; i < len; i++) {
    probs[i] *= expsum_reciprocal;
  }
}

// VectorFunctions_SSE2

#ifdef X64_SIMD_AVAILABLE

#if (defined(__GNUC__) || defined(__clang__))
#pragma GCC target("sse2")
#endif

// Static helper functions

static float horizontal_sum(__m128 sum_low, __m128 sum_high)
{
  __m128 sum128 = _mm_add_ps(sum_low, sum_high);
  sum128 = _mm_add_ps(sum128, _mm_movehl_ps(sum128, sum128));
  sum128 = _mm_add_ss(sum128, _mm_shuffle_ps(sum128, sum128, 0x55));
  float sum = _mm_cvtss_f32(sum128);
  return sum;
}

// Member implementations

void VectorFunctions_SSE2::Copy(float* dst, const float* src, size_t num_floats) {
  for (size_t i = 0; i < num_floats; i += 4) {
    __m128 vec = _mm_load_ps(src + i);
    _mm_store_ps(dst + i, vec);
  }
}

void VectorFunctions_SSE2::Zero(float* dst, size_t num_floats) {
  __m128 zeroes = _mm_setzero_ps();
  for (size_t i = 0; i < num_floats; i += 4) {
    _mm_store_ps(dst + i, zeroes);
  }
}

float VectorFunctions_SSE2::DotProduct(
  float const* x1,
  float const* x2,
  size_t const len)
{
  __m128 sum_low = _mm_setzero_ps();
  __m128 sum_high = _mm_setzero_ps();

  for (size_t i = 0; i < len; i += 8) {
    __m128 a0 = _mm_load_ps(x1 + i);
    __m128 b0 = _mm_load_ps(x2 + i);
    __m128 prod0 = _mm_mul_ps(a0, b0);
    sum_low = _mm_add_ps(sum_low, prod0);

    __m128 a1 = _mm_load_ps(x1 + i + 4);
    __m128 b1 = _mm_load_ps(x2 + i + 4);
    __m128 prod1 = _mm_mul_ps(a1, b1);
    sum_high = _mm_add_ps(sum_high, prod1);
  }

  return horizontal_sum(sum_low, sum_high);
}

float VectorFunctions_SSE2::SumOfSquares(float* array, size_t array_length)
{
  __m128 sum_vec0 = _mm_setzero_ps();
  __m128 sum_vec1 = _mm_setzero_ps();

  for (size_t i = 0; i < array_length; i += 8)
  {
    __m128 x0 = _mm_load_ps(&array[i]);
    sum_vec0 = _mm_add_ps(sum_vec0, _mm_mul_ps(x0, x0));
    __m128 x1 = _mm_load_ps(&array[i + 4]);
    sum_vec1 = _mm_add_ps(sum_vec1, _mm_mul_ps(x1, x1));
  }

  return horizontal_sum(sum_vec0, sum_vec1);
}

void VectorFunctions_SSE2::NormalizeThenActivate_Sigmoid(
  size_t array_length,
  float* to_be_normalized_values,
  float* activations_out,
  float* gamma,
  float* beta,
  float rms_scale)
{
  __m128 const c_half = _mm_set1_ps(0.5f);
  __m128 const c_27 = _mm_set1_ps(27.0f);
  __m128 const c_9 = _mm_set1_ps(9.0f);
  __m128 const c_clip_lower = _mm_set1_ps(SIGMOID_CLIP_MIN);
  __m128 const c_clip_upper = _mm_set1_ps(SIGMOID_CLIP_MAX);
  __m128 inv_var_vec = _mm_set1_ps(rms_scale);

  for (size_t i = 0; i < array_length; i += 4) {

    __m128 pre_activation_vec = _mm_load_ps(to_be_normalized_values + i);
    pre_activation_vec = _mm_mul_ps(pre_activation_vec, inv_var_vec);
    _mm_store_ps(to_be_normalized_values + i, pre_activation_vec);

    __m128 gamma_vec = _mm_load_ps(gamma + i);
    __m128 beta_vec = _mm_load_ps(beta + i);
    __m128 x = _mm_mul_ps(pre_activation_vec, gamma_vec);
    x = _mm_add_ps(x, beta_vec);

    // sigmoid

    x = _mm_max_ps(x, c_clip_lower);
    x = _mm_min_ps(x, c_clip_upper);

    __m128 u = _mm_mul_ps(x, c_half);
    __m128 u2 = _mm_mul_ps(u, u);

    __m128 numer = _mm_mul_ps(u, _mm_add_ps(c_27, u2));
    __m128 denom = _mm_add_ps(c_27, _mm_mul_ps(c_9, u2));
    __m128 tanh_val = _mm_div_ps(numer, denom);

    // sigmoid = 0.5 * (1 + tanh) = 0.5 + 0.5*tanh
    __m128 result = _mm_add_ps(c_half, _mm_mul_ps(c_half, tanh_val));

    _mm_store_ps(activations_out + i, result);
  }
}

void VectorFunctions_SSE2::NormalizeThenActivate_Tanh(
  size_t array_length,
  float* to_be_normalized_values,
  float* activations_out,
  float* gamma,
  float* beta,
  float rms_scale)
{
  __m128 const c_27 = _mm_set1_ps(27.0f);
  __m128 const c_9 = _mm_set1_ps(9.0f);
  __m128 const c_clip_lower = _mm_set1_ps(TANH_CLIP_MIN);
  __m128 const c_clip_upper = _mm_set1_ps(TANH_CLIP_MAX);
  __m128 inv_var_vec = _mm_set1_ps(rms_scale);

  for (size_t i = 0; i < array_length; i += 4) {

    __m128 pre_activation_vec = _mm_load_ps(to_be_normalized_values + i);
    pre_activation_vec = _mm_mul_ps(pre_activation_vec, inv_var_vec);
    _mm_store_ps(to_be_normalized_values + i, pre_activation_vec);

    __m128 gamma_vec = _mm_load_ps(gamma + i);
    __m128 beta_vec = _mm_load_ps(beta + i);
    __m128 x = _mm_mul_ps(pre_activation_vec, gamma_vec);
    x = _mm_add_ps(x, beta_vec);

    // tanh

    x = _mm_max_ps(x, c_clip_lower);
    x = _mm_min_ps(x, c_clip_upper);

    __m128 x2 = _mm_mul_ps(x, x);
    __m128 numer = _mm_mul_ps(x, _mm_add_ps(c_27, x2));
    __m128 denom = _mm_add_ps(c_27, _mm_mul_ps(c_9, x2));
    __m128 result = _mm_div_ps(numer, denom);

    _mm_store_ps(activations_out + i, result);
  }
}

void VectorFunctions_SSE2::AccumulateLstmGradients(
  size_t hidden_size,
  size_t concatenated_hidden_size,
  size_t vocabulary_size,
  size_t layer_id,
  float* error_on_output,
  float* hidden_gradient_accumulator,
  float* output_weights)
{
  size_t output_layer_offset = layer_id * hidden_size; // layer_id * 200

  for (size_t i = 0; i < vocabulary_size; i += 4) {   // 256 iterations, 4 at a time
    // Load 4 errors as a vector
    __m128 errors = _mm_load_ps(&error_on_output[i]);

    // Broadcast each error to its own vector
    __m128 error_vec0 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(0, 0, 0, 0));
    __m128 error_vec1 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(1, 1, 1, 1));
    __m128 error_vec2 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(2, 2, 2, 2));
    __m128 error_vec3 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(3, 3, 3, 3));

    for (size_t j = 0; j < hidden_size; j += 4) { // 200 iterations, 4 at a time
      size_t base_offset = output_layer_offset + j;

      // Load hidden_gradient_accumulatoronce
      __m128 hidden = _mm_load_ps(&hidden_gradient_accumulator[j]);

      // Load from 4 different output_weights rows and accumulate
      hidden = _mm_add_ps(hidden, _mm_mul_ps(_mm_load_ps(&output_weights[base_offset]), error_vec0)); base_offset += concatenated_hidden_size;
      hidden = _mm_add_ps(hidden, _mm_mul_ps(_mm_load_ps(&output_weights[base_offset]), error_vec1)); base_offset += concatenated_hidden_size;
      hidden = _mm_add_ps(hidden, _mm_mul_ps(_mm_load_ps(&output_weights[base_offset]), error_vec2)); base_offset += concatenated_hidden_size;
      hidden = _mm_add_ps(hidden, _mm_mul_ps(_mm_load_ps(&output_weights[base_offset]), error_vec3));

      // Store back to hidden_gradient_accumulator
      _mm_store_ps(&hidden_gradient_accumulator[j], hidden);
    }

    output_layer_offset += concatenated_hidden_size * 4;
  }
}

void VectorFunctions_SSE2::AccumulateLstmLayerGradients(
  size_t hidden_size,
  size_t timestep_offset,
  float* gradient_from_next_timestep,
  float* hidden_gradient_accumulator,
  float* tanh_state,
  float* forget_gate_activations,
  float* cell_candidate_activations,
  float* output_gate_activations,
  float* output_gate_gradients,
  float* cell_state_gradient,
  float* cell_candidate_gradients,
  float* forget_gate_gradients,
  float* last_cell_state)
{
  const __m128 ones = _mm_set1_ps(1.0f);
  const __m128 zeros = _mm_setzero_ps();

  for (size_t i = 0; i < hidden_size; i += 4) {
    __m128 stored_err = _mm_load_ps(&gradient_from_next_timestep[i]);
    __m128 hidden_err = _mm_load_ps(&hidden_gradient_accumulator[i]);

    // gradient_from_next_timestep[i] += hidden_gradient_accumulator[i]
    stored_err = _mm_add_ps(stored_err, hidden_err);
    _mm_store_ps(&gradient_from_next_timestep[i], stored_err);

    // hidden_gradient_accumulator[i] = 0.0f
    _mm_store_ps(&hidden_gradient_accumulator[i], zeros);

    // Load states from sequence_position offset
    const size_t idx = timestep_offset + i;
    __m128 tanh_v = _mm_load_ps(&tanh_state[idx]);
    __m128 forget_gate = _mm_load_ps(&forget_gate_activations[idx]);
    __m128 cell_candidtae = _mm_load_ps(&cell_candidate_activations[idx]);
    __m128 output_gate = _mm_load_ps(&output_gate_activations[idx]);
    __m128 input_gate = _mm_sub_ps(ones, forget_gate);

    // output_gate_gradients[i] = tanh_v * gradient_from_next_timestep[i] * output * (1.0f - output)
    __m128 one_minus_output = _mm_sub_ps(ones, output_gate);
    __m128 og_err = _mm_mul_ps(tanh_v, stored_err);
    og_err = _mm_mul_ps(og_err, output_gate);
    og_err = _mm_mul_ps(og_err, one_minus_output);
    _mm_store_ps(&output_gate_gradients[i], og_err);

    // cell_state_gradient[i] += gradient_from_next_timestep[i] * output * (1.0f - tanh_v * tanh_v)
    __m128 state_err = _mm_load_ps(&cell_state_gradient[i]);
    __m128 tanh_sq = _mm_mul_ps(tanh_v, tanh_v);
    __m128 one_minus_tanh_sq = _mm_sub_ps(ones, tanh_sq);
    __m128 temp = _mm_mul_ps(stored_err, output_gate);
    temp = _mm_mul_ps(temp, one_minus_tanh_sq);
    state_err = _mm_add_ps(state_err, temp);

    // cell_candidate_gradients[i] = cell_state_gradient[i] * input_gate * (1.0f - inputv * inputv)
    __m128 inputv_sq = _mm_mul_ps(cell_candidtae, cell_candidtae);
    __m128 one_minus_inputv_sq = _mm_sub_ps(ones, inputv_sq);
    __m128 ig_err = _mm_mul_ps(state_err, input_gate);
    ig_err = _mm_mul_ps(ig_err, one_minus_inputv_sq);
    _mm_store_ps(&cell_candidate_gradients[i], ig_err);

    // forget_gate_gradients[i] = (last_cell_state[idx] - inputv) * cell_state_gradient[i] * forget * input_gate
    __m128 last_st = _mm_load_ps(&last_cell_state[idx]);
    __m128 fg_err = _mm_sub_ps(last_st, cell_candidtae);
    fg_err = _mm_mul_ps(fg_err, state_err);
    fg_err = _mm_mul_ps(fg_err, forget_gate);
    fg_err = _mm_mul_ps(fg_err, input_gate);
    _mm_store_ps(&forget_gate_gradients[i], fg_err);

    if (timestep_offset > 0) { // sequence_position > 0
      state_err = _mm_mul_ps(state_err, forget_gate);
      _mm_store_ps(&gradient_from_next_timestep[i], zeros);
    }

    _mm_store_ps(&cell_state_gradient[i], state_err);
  }
}

void VectorFunctions_SSE2::BackpropagateErrors(
  size_t len,                       // hidden_size (200)
  size_t base_offset,               // 0 for temporal, hidden_size for spatial
  size_t component_input_dim,       // Layer 0: 200, Layer 1: 400
  float* weights,                   // Weight matrix
  float* pre_activation_gradients,  // Current layer errors
  float* grad_store)                // Where to accumulate gradients
{
  for (size_t i = 0; i < len; i += 8) {
    __m128 grad0_vec = _mm_load_ps(&grad_store[i]);
    __m128 grad1_vec = _mm_load_ps(&grad_store[i + 4]);

    // for better precision calculate the sum then add to the existing grads
    __m128 sum0 = _mm_setzero_ps();
    __m128 sum1 = _mm_setzero_ps();

    size_t weight_idx = base_offset + i;
    for (size_t i = 0; i < len; i++) {
      __m128 g = _mm_set1_ps(pre_activation_gradients[i]);

      __m128 w0 = _mm_load_ps(&weights[weight_idx]);
      __m128 prod0 = _mm_mul_ps(g, w0);
      sum0 = _mm_add_ps(sum0, prod0);

      __m128 w1 = _mm_load_ps(&weights[weight_idx + 4]);
      __m128 prod1 = _mm_mul_ps(g, w1);
      sum1 = _mm_add_ps(sum1, prod1);

      weight_idx += component_input_dim;
    }

    grad0_vec = _mm_add_ps(grad0_vec, sum0);
    grad1_vec = _mm_add_ps(grad1_vec, sum1);

    _mm_store_ps(&grad_store[i], grad0_vec);
    _mm_store_ps(&grad_store[i + 4], grad1_vec);
  }
}

void VectorFunctions_SSE2::AccumulateLayerGradients(
  const size_t hidden_size,
  const size_t vocabulary_size,
  const size_t component_input_dim,
  const float* input,
  const float* pre_activation_gradients,
  float* embedding_ptr,
  float* weight_gradients)
{
  for (size_t i = 0; i < hidden_size; i += 4) {
    // Load 4 errors as a vector
    __m128 errors = _mm_load_ps(&pre_activation_gradients[i]);

    // Broadcast each error
    __m128 error_vec0 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(0, 0, 0, 0));
    __m128 error_vec1 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(1, 1, 1, 1));
    __m128 error_vec2 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(2, 2, 2, 2));
    __m128 error_vec3 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(3, 3, 3, 3));

    // Extract scalar values from the broadcast vectors (just get first element)
    float e0 = _mm_cvtss_f32(error_vec0);
    float e1 = _mm_cvtss_f32(error_vec1);
    float e2 = _mm_cvtss_f32(error_vec2);
    float e3 = _mm_cvtss_f32(error_vec3);

    // Update symbol_embeddings gradient
    size_t emb_offset = i * vocabulary_size;
    embedding_ptr[emb_offset] += e0; emb_offset += vocabulary_size;
    embedding_ptr[emb_offset] += e1; emb_offset += vocabulary_size;
    embedding_ptr[emb_offset] += e2; emb_offset += vocabulary_size;
    embedding_ptr[emb_offset] += e3;

    // Update hidden state weight gradients
    size_t update_offset = i * component_input_dim;
    for (size_t j = 0; j < component_input_dim; j += 4) {
      size_t base_offset = update_offset + j;

      __m128 inp = _mm_load_ps(&input[j]);

      __m128 upd0 = _mm_load_ps(&weight_gradients[base_offset]);
      upd0 = _mm_add_ps(upd0, _mm_mul_ps(inp, error_vec0)); base_offset += component_input_dim;

      __m128 upd1 = _mm_load_ps(&weight_gradients[base_offset]);
      upd1 = _mm_add_ps(upd1, _mm_mul_ps(inp, error_vec1)); base_offset += component_input_dim;

      __m128 upd2 = _mm_load_ps(&weight_gradients[base_offset]);
      upd2 = _mm_add_ps(upd2, _mm_mul_ps(inp, error_vec2)); base_offset += component_input_dim;

      __m128 upd3 = _mm_load_ps(&weight_gradients[base_offset]);
      upd3 = _mm_add_ps(upd3, _mm_mul_ps(inp, error_vec3));

      base_offset = update_offset + j;
      _mm_store_ps(&weight_gradients[base_offset], upd0); base_offset += component_input_dim;
      _mm_store_ps(&weight_gradients[base_offset], upd1); base_offset += component_input_dim;
      _mm_store_ps(&weight_gradients[base_offset], upd2); base_offset += component_input_dim;
      _mm_store_ps(&weight_gradients[base_offset], upd3);
    }
  }
}

void VectorFunctions_SSE2::AccumulateOutputLayerGradients(
  size_t previous_output_offset,
  float* error_on_output,
  float* output_weight_gradients,
  float* output_bias_gradients,
  const float* hidden_ptr,
  const size_t vocabulary_size,
  const size_t concatenated_hidden_size,
  const size_t input_symbol)
{
  for (size_t i = 0; i < vocabulary_size; i += 4) {
    // Load 4 errors
    __m128 errors = _mm_load_ps(&error_on_output[i]);

    // Broadcast each error
    __m128 error_vec0 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(0, 0, 0, 0));
    __m128 error_vec1 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(1, 1, 1, 1));
    __m128 error_vec2 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(2, 2, 2, 2));
    __m128 error_vec3 = _mm_shuffle_ps(errors, errors, _MM_SHUFFLE(3, 3, 3, 3));

    // Update bias (vectorized)
    __m128 bias = _mm_load_ps(&output_bias_gradients[i]);
    bias = _mm_add_ps(bias, errors);
    _mm_store_ps(&output_bias_gradients[i], bias);

    // Update output layer weights
    size_t output_offset = i * concatenated_hidden_size;
    for (size_t j = 0; j < concatenated_hidden_size; j += 4) {
      size_t base_offset = output_offset + j;

      __m128 hidden = _mm_load_ps(&hidden_ptr[j]);

      __m128 out = _mm_load_ps(&output_weight_gradients[base_offset]);
      out = _mm_add_ps(out, _mm_mul_ps(hidden, error_vec0)); base_offset += concatenated_hidden_size;

      __m128 out1 = _mm_load_ps(&output_weight_gradients[base_offset]);
      out1 = _mm_add_ps(out1, _mm_mul_ps(hidden, error_vec1)); base_offset += concatenated_hidden_size;

      __m128 out2 = _mm_load_ps(&output_weight_gradients[base_offset]);
      out2 = _mm_add_ps(out2, _mm_mul_ps(hidden, error_vec2)); base_offset += concatenated_hidden_size;

      __m128 out3 = _mm_load_ps(&output_weight_gradients[base_offset]);
      out3 = _mm_add_ps(out3, _mm_mul_ps(hidden, error_vec3));

      base_offset = output_offset + j;
      _mm_store_ps(&output_weight_gradients[base_offset], out); base_offset += concatenated_hidden_size;
      _mm_store_ps(&output_weight_gradients[base_offset], out1); base_offset += concatenated_hidden_size;
      _mm_store_ps(&output_weight_gradients[base_offset], out2); base_offset += concatenated_hidden_size;
      _mm_store_ps(&output_weight_gradients[base_offset], out3);
    }
  }
}

float VectorFunctions_SSE2::ComputeMaxLogit(
  float* result,
  size_t result_length)
{
  __m128 max_logit_vec = _mm_set1_ps(negative_infinity);

  for (size_t i = 0; i < result_length; i += 16) {
    __m128 v0 = _mm_load_ps(result + i + 0);
    __m128 v1 = _mm_load_ps(result + i + 4);
    __m128 v2 = _mm_load_ps(result + i + 8);
    __m128 v3 = _mm_load_ps(result + i + 12);

    max_logit_vec = _mm_max_ps(max_logit_vec, v0);
    max_logit_vec = _mm_max_ps(max_logit_vec, v1);
    max_logit_vec = _mm_max_ps(max_logit_vec, v2);
    max_logit_vec = _mm_max_ps(max_logit_vec, v3);
  }

  // Perform horizontal reduction to find the max value
  __m128 shuf = _mm_shuffle_ps(max_logit_vec, max_logit_vec, _MM_SHUFFLE(2, 3, 0, 1));
  max_logit_vec = _mm_max_ps(max_logit_vec, shuf);
  shuf = _mm_shuffle_ps(max_logit_vec, max_logit_vec, _MM_SHUFFLE(1, 0, 3, 2));
  max_logit_vec = _mm_max_ps(max_logit_vec, shuf);
  shuf = _mm_shuffle_ps(max_logit_vec, max_logit_vec, _MM_SHUFFLE(0, 1, 2, 3));
  max_logit_vec = _mm_max_ps(max_logit_vec, shuf);

  float maxlogit = _mm_cvtss_f32(max_logit_vec);
  return maxlogit;
}

void VectorFunctions_SSE2::MatvecThenSoftmax(
  float* hidden,
  float* logits,
  float* output_weights,
  float* output,
  float* output_bias,
  size_t const concatenated_hidden_size,
  size_t const vocabulary_size,
  size_t const output_offset)
{
  // Compute logits via dot products
  for (size_t i = 0; i < vocabulary_size; i++) {
    logits[output_offset + i] = DotProduct(
      &hidden[0],
      &output_weights[i * concatenated_hidden_size],
      concatenated_hidden_size
    ) + output_bias[i];
  }

  // Find max logit for numerical stability
  float max_logit = ComputeMaxLogit(&logits[output_offset], vocabulary_size);

  // Compute softmax
  Softmax(
    &logits[output_offset],
    &output[output_offset],
    vocabulary_size,
    max_logit);
}


void VectorFunctions_SSE2::Softmax(
  float* logits,
  float* probs,
  size_t len,
  float max_logit)
{
  // Constants for exponential
  const __m128 INV_LN2 = _mm_set1_ps(0x1.715476p+0f);
  const __m128 LN2_HI = _mm_set1_ps(0x1.62e400p-1f);
  const __m128 LN2_LO = _mm_set1_ps(0x1.7f7d1cp-20f);
  const __m128 c2 = _mm_set1_ps(0x1.fffe24p-2f);
  const __m128 c3 = _mm_set1_ps(0x1.5554acp-3f);
  const __m128 c4 = _mm_set1_ps(0x1.5713a4p-5f);
  const __m128 c5 = _mm_set1_ps(0x1.12266ap-7f);
  const __m128 one_vec = _mm_set1_ps(1.0f);
  const __m128i MAGIC_BIAS = _mm_set1_epi32(12582912);
  const __m128 MAGIC_BIAS_FLOAT = _mm_set1_ps(12582912.0f);
  const __m128i c127 = _mm_set1_epi32(127);
  const __m128 cplus87 = _mm_set1_ps(87.0f);
  const __m128 cminus87 = _mm_set1_ps(-87.0f);
  const __m128 maxlogit_vec = _mm_set1_ps(max_logit);

  __m128 expsum_vec1 = _mm_setzero_ps();
  __m128 expsum_vec2 = _mm_setzero_ps();

  for (size_t i = 0; i < len;)
  {
    // First block of 4
    {
      //prepare
      __m128 logits_vec = _mm_load_ps(&logits[i]);
      logits_vec = _mm_sub_ps(logits_vec, maxlogit_vec);

      //exp
      logits_vec = _mm_min_ps(logits_vec, cplus87);
      logits_vec = _mm_max_ps(logits_vec, cminus87);

      __m128 z = _mm_mul_ps(logits_vec, INV_LN2);

      __m128 t = _mm_add_ps(z, MAGIC_BIAS_FLOAT);
      __m128i n = _mm_sub_epi32(_mm_cvttps_epi32(t), MAGIC_BIAS);
      t = _mm_sub_ps(t, MAGIC_BIAS_FLOAT);

      __m128 r = _mm_sub_ps(logits_vec, _mm_mul_ps(t, LN2_HI));
      r = _mm_sub_ps(r, _mm_mul_ps(t, LN2_LO));

      __m128 r2 = _mm_mul_ps(r, r);
      __m128 q2 = _mm_add_ps(c2, _mm_mul_ps(c3, r));
      __m128 q4 = _mm_add_ps(c4, _mm_mul_ps(c5, r));
      __m128 p = _mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(q4, r2), q2), r2), _mm_add_ps(r, one_vec));

      __m128i expbits = _mm_add_epi32(n, c127);
      __m128i bits = _mm_slli_epi32(expbits, 23);
      __m128 two_n = _mm_castsi128_ps(bits);

      __m128 result_vec = _mm_mul_ps(p, two_n);

      //finalize, store
      _mm_store_ps(&probs[i], result_vec);
      expsum_vec1 = _mm_add_ps(expsum_vec1, result_vec);
    }
    i += 4;

    // Second block of 4

    {
      //prepare
      __m128 logits_vec = _mm_load_ps(&logits[i]);
      logits_vec = _mm_sub_ps(logits_vec, maxlogit_vec);

      //exp
      logits_vec = _mm_min_ps(logits_vec, cplus87);
      logits_vec = _mm_max_ps(logits_vec, cminus87);

      __m128 z = _mm_mul_ps(logits_vec, INV_LN2);

      __m128 t = _mm_add_ps(z, MAGIC_BIAS_FLOAT);
      __m128i n = _mm_sub_epi32(_mm_cvttps_epi32(t), MAGIC_BIAS);
      t = _mm_sub_ps(t, MAGIC_BIAS_FLOAT);

      __m128 r = _mm_sub_ps(logits_vec, _mm_mul_ps(t, LN2_HI));
      r = _mm_sub_ps(r, _mm_mul_ps(t, LN2_LO));

      __m128 r2 = _mm_mul_ps(r, r);
      __m128 q2 = _mm_add_ps(c2, _mm_mul_ps(c3, r));
      __m128 q4 = _mm_add_ps(c4, _mm_mul_ps(c5, r));
      __m128 p = _mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(q4, r2), q2), r2), _mm_add_ps(r, one_vec));

      __m128i expbits = _mm_add_epi32(n, c127);
      __m128i bits = _mm_slli_epi32(expbits, 23);
      __m128  two_n = _mm_castsi128_ps(bits);

      __m128 result_vec = _mm_mul_ps(p, two_n);

      //finalize, store
      _mm_store_ps(&probs[i], result_vec);
      expsum_vec2 = _mm_add_ps(expsum_vec2, result_vec);
    }

    i += 4;
  }

  float expsum = horizontal_sum(expsum_vec1, expsum_vec2);
  float expsum_reciprocal = 1.0f / expsum;
  __m128 expsum_reciprocal_vec = _mm_set1_ps(expsum_reciprocal);

  for (size_t i = 0; i < len; i += 4)
  {
    __m128 softmax_probs_vec = _mm_load_ps(&probs[i]);
    __m128 result_vec = _mm_mul_ps(softmax_probs_vec, expsum_reciprocal_vec);
    _mm_store_ps(&probs[i], result_vec);
  }
}

#endif

// VectorFunctions_AVX2

#ifdef X64_SIMD_AVAILABLE

#if (defined(__GNUC__) || defined(__clang__))
#define AVX2_TARGET __attribute__((target("avx2")))
#else
#define AVX2_TARGET
#endif

// Static helper functions

AVX2_TARGET
static float horizontal_sum(__m256 sum_vec)
{
  __m128 sum_high = _mm256_extractf128_ps(sum_vec, 1);
  __m128 sum_low = _mm256_castps256_ps128(sum_vec);
  __m128 sum128 = _mm_add_ps(sum_low, sum_high);
  sum128 = _mm_add_ps(sum128, _mm_movehl_ps(sum128, sum128));
  sum128 = _mm_add_ss(sum128, _mm_shuffle_ps(sum128, sum128, 0x55));
  float sum = _mm_cvtss_f32(sum128);
  return sum;
}

// Member implementations

AVX2_TARGET
void VectorFunctions_AVX2::Copy(float* dst, const float* src, size_t num_floats) {
  for (size_t i = 0; i < num_floats; i += 8) {
    __m256 vec = _mm256_load_ps(src + i);
    _mm256_store_ps(dst + i, vec);
  }
}

AVX2_TARGET
void VectorFunctions_AVX2::Zero(float* dst, size_t num_floats) {
  __m256 zeroes = _mm256_setzero_ps();
  for (size_t i = 0; i < num_floats; i += 8) {
    _mm256_store_ps(dst + i, zeroes);
  }
}

AVX2_TARGET
float VectorFunctions_AVX2::DotProduct(
  float const* x1,
  float const* x2,
  size_t const len)
{
  __m256 sum = _mm256_setzero_ps();

  for (size_t i = 0; i < len; i += 8) {
    __m256 a = _mm256_load_ps(x1 + i);
    __m256 b = _mm256_load_ps(x2 + i);
    __m256 prod = _mm256_mul_ps(a, b);
    sum = _mm256_add_ps(sum, prod);
  }

  return horizontal_sum(sum);
}

AVX2_TARGET
float VectorFunctions_AVX2::SumOfSquares(float* array, size_t array_length) {
  __m256 sum_vec = _mm256_setzero_ps();

  for (size_t i = 0; i < array_length; i += 8) {
    __m256 x = _mm256_load_ps(&array[i]);
    sum_vec = _mm256_add_ps(sum_vec, _mm256_mul_ps(x, x));
  }
  return horizontal_sum(sum_vec);
}

AVX2_TARGET
void VectorFunctions_AVX2::NormalizeThenActivate_Sigmoid(
  size_t array_length,
  float* to_be_normalized_values,
  float* activations_out,
  float* gamma,
  float* beta,
  float rms_scale)
{
  __m256 const c_half = _mm256_set1_ps(0.5f);
  __m256 const c_27 = _mm256_set1_ps(27.0f);
  __m256 const c_9 = _mm256_set1_ps(9.0f);
  __m256 const c_clip_lower = _mm256_set1_ps(SIGMOID_CLIP_MIN);
  __m256 const c_clip_upper = _mm256_set1_ps(SIGMOID_CLIP_MAX);
  __m256 inv_var_vec = _mm256_set1_ps(rms_scale);

  for (size_t i = 0; i < array_length; i += 8) {

    __m256 pre_activation_vec = _mm256_load_ps(to_be_normalized_values + i);
    pre_activation_vec = _mm256_mul_ps(pre_activation_vec, inv_var_vec);
    _mm256_store_ps(to_be_normalized_values + i, pre_activation_vec);

    __m256 gamma_vec = _mm256_load_ps(gamma + i);
    __m256 beta_vec = _mm256_load_ps(beta + i);
    __m256 x = _mm256_mul_ps(pre_activation_vec, gamma_vec);
    x = _mm256_add_ps(x, beta_vec);

    // sigmoid

    x = _mm256_max_ps(x, c_clip_lower);
    x = _mm256_min_ps(x, c_clip_upper);

    __m256 u = _mm256_mul_ps(x, c_half);
    __m256 u2 = _mm256_mul_ps(u, u);

    __m256 numer = _mm256_mul_ps(u, _mm256_add_ps(c_27, u2));
    __m256 denom = _mm256_add_ps(c_27, _mm256_mul_ps(c_9, u2));
    __m256 tanh_val = _mm256_div_ps(numer, denom);

    // sigmoid = 0.5 * (1 + tanh) = 0.5 + 0.5*tanh
    __m256 result = _mm256_add_ps(c_half, _mm256_mul_ps(c_half, tanh_val));

    _mm256_store_ps(activations_out + i, result);
  }
}

AVX2_TARGET
void VectorFunctions_AVX2::NormalizeThenActivate_Tanh(
  size_t array_length,
  float* to_be_normalized_values,
  float* activations_out,
  float* gamma,
  float* beta,
  float rms_scale)
{
  __m256 const c_27 = _mm256_set1_ps(27.0f);
  __m256 const c_9 = _mm256_set1_ps(9.0f);
  __m256 const c_clip_lower = _mm256_set1_ps(TANH_CLIP_MIN);
  __m256 const c_clip_upper = _mm256_set1_ps(TANH_CLIP_MAX);
  __m256 inv_var_vec = _mm256_set1_ps(rms_scale);

  for (size_t i = 0; i < array_length; i += 8) {

    __m256 pre_activation_vec = _mm256_load_ps(to_be_normalized_values + i);
    pre_activation_vec = _mm256_mul_ps(pre_activation_vec, inv_var_vec);
    _mm256_store_ps(to_be_normalized_values + i, pre_activation_vec);

    __m256 gamma_vec = _mm256_load_ps(gamma + i);
    __m256 beta_vec = _mm256_load_ps(beta + i);
    __m256 x = _mm256_mul_ps(pre_activation_vec, gamma_vec);
    x = _mm256_add_ps(x, beta_vec);

    // tanh

    x = _mm256_max_ps(x, c_clip_lower);
    x = _mm256_min_ps(x, c_clip_upper);

    __m256 x2 = _mm256_mul_ps(x, x);
    __m256 numer = _mm256_mul_ps(x, _mm256_add_ps(c_27, x2));
    __m256 denom = _mm256_add_ps(c_27, _mm256_mul_ps(c_9, x2));
    __m256 result = _mm256_div_ps(numer, denom);

    _mm256_store_ps(activations_out + i, result);
  }
}

AVX2_TARGET
void VectorFunctions_AVX2::AccumulateLstmGradients(
  size_t hidden_size,
  size_t concatenated_hidden_size,
  size_t vocabulary_size,
  size_t layer_id,
  float* error_on_output,
  float* hidden_gradient_accumulator,
  float* output_weights)
{
  size_t output_layer_offset = layer_id * hidden_size; // layer_id * 200

  for (size_t i = 0; i < vocabulary_size; i += 8) {   // 256 iterations, 8 at a time
    // Load 8 errors as a vector
    __m256 errors = _mm256_load_ps(&error_on_output[i]);

    // Broadcast each error to its own vector using AVX2 permutevar8x32
    __m256 error_vec0 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(0));
    __m256 error_vec1 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(1));
    __m256 error_vec2 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(2));
    __m256 error_vec3 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(3));
    __m256 error_vec4 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(4));
    __m256 error_vec5 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(5));
    __m256 error_vec6 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(6));
    __m256 error_vec7 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(7));

    for (size_t j = 0; j < hidden_size; j += 8) { // 200 iterations, 8 at a time
      size_t base_offset = output_layer_offset + j;

      // Load hidden_gradient_accumulatoronce
      __m256 hidden = _mm256_load_ps(&hidden_gradient_accumulator[j]);

      // Load from 8 different output_weights rows and accumulate
      hidden = _mm256_add_ps(hidden, _mm256_mul_ps(_mm256_load_ps(&output_weights[base_offset]), error_vec0)); base_offset += concatenated_hidden_size;
      hidden = _mm256_add_ps(hidden, _mm256_mul_ps(_mm256_load_ps(&output_weights[base_offset]), error_vec1)); base_offset += concatenated_hidden_size;
      hidden = _mm256_add_ps(hidden, _mm256_mul_ps(_mm256_load_ps(&output_weights[base_offset]), error_vec2)); base_offset += concatenated_hidden_size;
      hidden = _mm256_add_ps(hidden, _mm256_mul_ps(_mm256_load_ps(&output_weights[base_offset]), error_vec3)); base_offset += concatenated_hidden_size;
      hidden = _mm256_add_ps(hidden, _mm256_mul_ps(_mm256_load_ps(&output_weights[base_offset]), error_vec4)); base_offset += concatenated_hidden_size;
      hidden = _mm256_add_ps(hidden, _mm256_mul_ps(_mm256_load_ps(&output_weights[base_offset]), error_vec5)); base_offset += concatenated_hidden_size;
      hidden = _mm256_add_ps(hidden, _mm256_mul_ps(_mm256_load_ps(&output_weights[base_offset]), error_vec6)); base_offset += concatenated_hidden_size;
      hidden = _mm256_add_ps(hidden, _mm256_mul_ps(_mm256_load_ps(&output_weights[base_offset]), error_vec7));

      // Store back to hidden_gradient_accumulator
      _mm256_store_ps(&hidden_gradient_accumulator[j], hidden);
    }

    output_layer_offset += concatenated_hidden_size * 8;
  }
}

AVX2_TARGET
void VectorFunctions_AVX2::AccumulateLstmLayerGradients(
  size_t hidden_size,
  size_t timestep_offset,
  float* gradient_from_next_timestep,
  float* hidden_gradient_accumulator,
  float* tanh_state,
  float* forget_gate_activations,
  float* cell_candidate_activations,
  float* output_gate_activations,
  float* output_gate_gradients,
  float* cell_state_gradient,
  float* cell_candidate_gradients,
  float* forget_gate_gradients,
  float* last_cell_state)
{
  const __m256 ones = _mm256_set1_ps(1.0f);
  const __m256 zeros = _mm256_setzero_ps();

  for (size_t i = 0; i < hidden_size; i += 8) {
    __m256 stored_err = _mm256_load_ps(&gradient_from_next_timestep[i]);
    __m256 hidden_err = _mm256_load_ps(&hidden_gradient_accumulator[i]);

    // gradient_from_next_timestep[i] += hidden_gradient_accumulator[i]
    stored_err = _mm256_add_ps(stored_err, hidden_err);
    _mm256_store_ps(&gradient_from_next_timestep[i], stored_err);

    // hidden_gradient_accumulator[i] = 0.0f
    _mm256_store_ps(&hidden_gradient_accumulator[i], zeros);

    // Load states from sequence_position offset
    const size_t idx = timestep_offset + i;
    __m256 tanh_v = _mm256_load_ps(&tanh_state[idx]);
    __m256 forget_gate = _mm256_load_ps(&forget_gate_activations[idx]);
    __m256 cell_candidate = _mm256_load_ps(&cell_candidate_activations[idx]);
    __m256 output_gate = _mm256_load_ps(&output_gate_activations[idx]);
    __m256 input_gate = _mm256_sub_ps(ones, forget_gate);

    // output_gate_gradients[i] = tanh_v * gradient_from_next_timestep[i] * output * (1.0f - output)
    __m256 one_minus_output = _mm256_sub_ps(ones, output_gate);
    __m256 og_err = _mm256_mul_ps(tanh_v, stored_err);
    og_err = _mm256_mul_ps(og_err, output_gate);
    og_err = _mm256_mul_ps(og_err, one_minus_output);
    _mm256_store_ps(&output_gate_gradients[i], og_err);

    // cell_state_gradient[i] += gradient_from_next_timestep[i] * output * (1.0f - tanh_v * tanh_v)
    __m256 state_err = _mm256_load_ps(&cell_state_gradient[i]);
    __m256 tanh_sq = _mm256_mul_ps(tanh_v, tanh_v);
    __m256 one_minus_tanh_sq = _mm256_sub_ps(ones, tanh_sq);
    __m256 temp = _mm256_mul_ps(stored_err, output_gate);
    temp = _mm256_mul_ps(temp, one_minus_tanh_sq);
    state_err = _mm256_add_ps(state_err, temp);

    // cell_candidate_gradients[i] = cell_state_gradient[i] * input_gate * (1.0f - inputv * inputv)
    __m256 inputv_sq = _mm256_mul_ps(cell_candidate, cell_candidate);
    __m256 one_minus_inputv_sq = _mm256_sub_ps(ones, inputv_sq);
    __m256 ig_err = _mm256_mul_ps(state_err, input_gate);
    ig_err = _mm256_mul_ps(ig_err, one_minus_inputv_sq);
    _mm256_store_ps(&cell_candidate_gradients[i], ig_err);

    // forget_gate_gradients[i] = (last_cell_state[idx] - inputv) * cell_state_gradient[i] * forget * input_gate
    __m256 last_st = _mm256_load_ps(&last_cell_state[idx]);
    __m256 fg_err = _mm256_sub_ps(last_st, cell_candidate);
    fg_err = _mm256_mul_ps(fg_err, state_err);
    fg_err = _mm256_mul_ps(fg_err, forget_gate);
    fg_err = _mm256_mul_ps(fg_err, input_gate);
    _mm256_store_ps(&forget_gate_gradients[i], fg_err);

    if (timestep_offset > 0) { // sequence_position > 0
      state_err = _mm256_mul_ps(state_err, forget_gate);
      _mm256_store_ps(&gradient_from_next_timestep[i], zeros);
    }

    _mm256_store_ps(&cell_state_gradient[i], state_err);
  }
}

AVX2_TARGET
void VectorFunctions_AVX2::BackpropagateErrors(
  size_t len,                       // hidden_size (200)
  size_t base_offset,               // 0 for temporal, hidden_size for spatial
  size_t component_input_dim,       // Layer 0: 200, Layer 1: 400
  float* weights,                   // Weight matrix
  float* pre_activation_gradients,  // Current layer errors
  float* grad_store)                // Where to accumulate gradients
{
  for (size_t i = 0; i < len; i += 8)
  {
    __m256 grad_vec = _mm256_load_ps(&grad_store[i]);

    // for better precision calculate the sum then add to the existing grads
    __m256 sum = _mm256_setzero_ps();

    size_t weight_idx = base_offset + i;
    for (size_t i = 0; i < len; i++) {
      __m256 g = _mm256_set1_ps(pre_activation_gradients[i]);
      __m256 w = _mm256_load_ps(&weights[weight_idx]);
      __m256 prod = _mm256_mul_ps(g, w);
      sum = _mm256_add_ps(sum, prod);
      weight_idx += component_input_dim;
    }

    grad_vec = _mm256_add_ps(grad_vec, sum);
    _mm256_store_ps(&grad_store[i], grad_vec);
  }
}

AVX2_TARGET
void VectorFunctions_AVX2::AccumulateLayerGradients(
  const size_t hidden_size,
  const size_t vocabulary_size,
  const size_t component_input_dim,
  const float* input,
  const float* pre_activation_gradients,
  float* embedding_ptr,
  float* weight_gradients)
{
  for (size_t i = 0; i < hidden_size; i += 4) {
    // Load 4 errors (using only lower half of AVX register)
    __m128 errors_128 = _mm_load_ps(&pre_activation_gradients[i]);
    __m256 errors = _mm256_castps128_ps256(errors_128);

    // Broadcast each error
    __m256 error_vec0 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(0));
    __m256 error_vec1 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(1));
    __m256 error_vec2 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(2));
    __m256 error_vec3 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(3));

    // Extract scalar values from the broadcast vectors (just get first element)
    float e0 = _mm256_cvtss_f32(error_vec0);
    float e1 = _mm256_cvtss_f32(error_vec1);
    float e2 = _mm256_cvtss_f32(error_vec2);
    float e3 = _mm256_cvtss_f32(error_vec3);

    // Update symbol_embeddings gradient
    size_t emb_offset = i * vocabulary_size;
    embedding_ptr[emb_offset] += e0; emb_offset += vocabulary_size;
    embedding_ptr[emb_offset] += e1; emb_offset += vocabulary_size;
    embedding_ptr[emb_offset] += e2; emb_offset += vocabulary_size;
    embedding_ptr[emb_offset] += e3;

    // Update hidden state weight gradients
    size_t update_offset = i * component_input_dim;
    for (size_t j = 0; j < component_input_dim; j += 8) {
      size_t base_offset = update_offset + j;

      __m256 inp = _mm256_load_ps(&input[j]);

      __m256 upd0 = _mm256_load_ps(&weight_gradients[base_offset]);
      upd0 = _mm256_add_ps(upd0, _mm256_mul_ps(inp, error_vec0)); base_offset += component_input_dim;

      __m256 upd1 = _mm256_load_ps(&weight_gradients[base_offset]);
      upd1 = _mm256_add_ps(upd1, _mm256_mul_ps(inp, error_vec1)); base_offset += component_input_dim;

      __m256 upd2 = _mm256_load_ps(&weight_gradients[base_offset]);
      upd2 = _mm256_add_ps(upd2, _mm256_mul_ps(inp, error_vec2)); base_offset += component_input_dim;

      __m256 upd3 = _mm256_load_ps(&weight_gradients[base_offset]);
      upd3 = _mm256_add_ps(upd3, _mm256_mul_ps(inp, error_vec3));

      base_offset = update_offset + j;
      _mm256_store_ps(&weight_gradients[base_offset], upd0); base_offset += component_input_dim;
      _mm256_store_ps(&weight_gradients[base_offset], upd1); base_offset += component_input_dim;
      _mm256_store_ps(&weight_gradients[base_offset], upd2); base_offset += component_input_dim;
      _mm256_store_ps(&weight_gradients[base_offset], upd3);
    }
  }
}

AVX2_TARGET
void VectorFunctions_AVX2::AccumulateOutputLayerGradients(
  size_t previous_output_offset,
  float* error_on_output,
  float* output_weight_gradients,
  float* output_bias_gradients,
  const float* hidden_ptr,
  const size_t vocabulary_size,
  const size_t concatenated_hidden_size,
  const size_t input_symbol)
{
  for (size_t i = 0; i < vocabulary_size; i += 4) {
    // Load 4 errors
    __m128 errors_128 = _mm_load_ps(&error_on_output[i]);
    __m256 errors = _mm256_castps128_ps256(errors_128);

    // Broadcast each error
    __m256 error_vec0 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(0));
    __m256 error_vec1 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(1));
    __m256 error_vec2 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(2));
    __m256 error_vec3 = _mm256_permutevar8x32_ps(errors, _mm256_set1_epi32(3));

    // Update bias (vectorized)
    __m128 bias = _mm_load_ps(&output_bias_gradients[i]);
    bias = _mm_add_ps(bias, errors_128);
    _mm_store_ps(&output_bias_gradients[i], bias);

    // Update output layer weights
    size_t output_offset = i * concatenated_hidden_size;
    for (size_t j = 0; j < concatenated_hidden_size; j += 8) {
      size_t base_offset = output_offset + j;

      __m256 hidden = _mm256_load_ps(&hidden_ptr[j]);

      __m256 out = _mm256_load_ps(&output_weight_gradients[base_offset]);
      out = _mm256_add_ps(out, _mm256_mul_ps(hidden, error_vec0)); base_offset += concatenated_hidden_size;

      __m256 out1 = _mm256_load_ps(&output_weight_gradients[base_offset]);
      out1 = _mm256_add_ps(out1, _mm256_mul_ps(hidden, error_vec1)); base_offset += concatenated_hidden_size;

      __m256 out2 = _mm256_load_ps(&output_weight_gradients[base_offset]);
      out2 = _mm256_add_ps(out2, _mm256_mul_ps(hidden, error_vec2)); base_offset += concatenated_hidden_size;

      __m256 out3 = _mm256_load_ps(&output_weight_gradients[base_offset]);
      out3 = _mm256_add_ps(out3, _mm256_mul_ps(hidden, error_vec3));

      base_offset = output_offset + j;
      _mm256_store_ps(&output_weight_gradients[base_offset], out); base_offset += concatenated_hidden_size;
      _mm256_store_ps(&output_weight_gradients[base_offset], out1); base_offset += concatenated_hidden_size;
      _mm256_store_ps(&output_weight_gradients[base_offset], out2); base_offset += concatenated_hidden_size;
      _mm256_store_ps(&output_weight_gradients[base_offset], out3);
    }
  }
}

AVX2_TARGET
float VectorFunctions_AVX2::ComputeMaxLogit(
  float* result,
  size_t result_length
)
{
  __m256 max_logit_vec = _mm256_set1_ps(negative_infinity);

  for (size_t i = 0; i < result_length; i += 16) {
    __m256 v0 = _mm256_load_ps(result + i);
    __m256 v1 = _mm256_load_ps(result + i + 8);

    max_logit_vec = _mm256_max_ps(max_logit_vec, v0);
    max_logit_vec = _mm256_max_ps(max_logit_vec, v1);
  }

  // Perform horizontal reduction to find the max value
  __m256 shuf = _mm256_permute2f128_ps(max_logit_vec, max_logit_vec, 1);
  max_logit_vec = _mm256_max_ps(max_logit_vec, shuf);
  max_logit_vec = _mm256_max_ps(max_logit_vec, _mm256_permute_ps(max_logit_vec, _MM_SHUFFLE(2, 3, 0, 1)));
  max_logit_vec = _mm256_max_ps(max_logit_vec, _mm256_permute_ps(max_logit_vec, _MM_SHUFFLE(1, 0, 3, 2)));

  float maxlogit = _mm_cvtss_f32(_mm256_castps256_ps128(max_logit_vec));
  return maxlogit;
}

AVX2_TARGET
void VectorFunctions_AVX2::MatvecThenSoftmax(
  float* hidden,
  float* logits,
  float* output_weights,
  float* output,
  float* output_bias,
  size_t const all_layer_inputs, // 200*2 = 400
  size_t const vocabulary_size, // 256
  size_t const output_offset)
{
  // Compute logits via dot products
  for (size_t i = 0; i < vocabulary_size; i++) {  // 256 iterations
    logits[output_offset + i] = DotProduct(
      &hidden[0],
      &output_weights[i * all_layer_inputs],
      all_layer_inputs // 400
    ) + output_bias[i];
  }

  // Find max logit for numerical stability
  float max_logit = ComputeMaxLogit(&logits[output_offset], vocabulary_size);

  // Compute softmax
  Softmax(
    &logits[output_offset],
    &output[output_offset],
    vocabulary_size,  // 256
    max_logit);
}

AVX2_TARGET
void VectorFunctions_AVX2::Softmax(
  float* logits,
  float* probs,
  size_t len,
  float max_logit)
{
  // Constants for exponential
  const __m256 INV_LN2 = _mm256_set1_ps(0x1.715476p+0f);
  const __m256 LN2_HI = _mm256_set1_ps(0x1.62e400p-1f);
  const __m256 LN2_LO = _mm256_set1_ps(0x1.7f7d1cp-20f);
  const __m256 c2 = _mm256_set1_ps(0x1.fffe24p-2f);
  const __m256 c3 = _mm256_set1_ps(0x1.5554acp-3f);
  const __m256 c4 = _mm256_set1_ps(0x1.5713a4p-5f);
  const __m256 c5 = _mm256_set1_ps(0x1.12266ap-7f);
  const __m256 one_vec = _mm256_set1_ps(1.0f);
  const __m256i MAGIC_BIAS = _mm256_set1_epi32(12582912);
  const __m256 MAGIC_BIAS_FLOAT = _mm256_set1_ps(12582912.0f);
  const __m256i c127 = _mm256_set1_epi32(127);
  const __m256 cplus87 = _mm256_set1_ps(87.0f);
  const __m256 cminus87 = _mm256_set1_ps(-87.0f);
  const __m256 maxlogit_vec = _mm256_set1_ps(max_logit);

  __m256 expsum_vec = _mm256_setzero_ps();

  for (size_t i = 0; i < len; i += 8)
  {
    //prepare
    __m256 logits_vec = _mm256_load_ps(&logits[i]);
    logits_vec = _mm256_sub_ps(logits_vec, maxlogit_vec);

    //exp
    logits_vec = _mm256_min_ps(logits_vec, cplus87);
    logits_vec = _mm256_max_ps(logits_vec, cminus87);

    __m256 z = _mm256_mul_ps(logits_vec, INV_LN2);

    __m256 t = _mm256_add_ps(z, MAGIC_BIAS_FLOAT);
    __m256i n = _mm256_sub_epi32(_mm256_cvttps_epi32(t), MAGIC_BIAS);
    t = _mm256_sub_ps(t, MAGIC_BIAS_FLOAT);

    __m256 r = _mm256_sub_ps(logits_vec, _mm256_mul_ps(t, LN2_HI));
    r = _mm256_sub_ps(r, _mm256_mul_ps(t, LN2_LO));

    __m256 r2 = _mm256_mul_ps(r, r);
    __m256 q2 = _mm256_add_ps(c2, _mm256_mul_ps(c3, r));
    __m256 q4 = _mm256_add_ps(c4, _mm256_mul_ps(c5, r));
    __m256 p = _mm256_add_ps(_mm256_mul_ps(_mm256_add_ps(_mm256_mul_ps(q4, r2), q2), r2), _mm256_add_ps(r, one_vec));

    __m256i expbits = _mm256_add_epi32(n, c127);
    __m256i bits = _mm256_slli_epi32(expbits, 23);
    __m256 two_n = _mm256_castsi256_ps(bits);

    __m256 result_vec = _mm256_mul_ps(p, two_n);

    //finalize, store
    _mm256_store_ps(&probs[i], result_vec);
    expsum_vec = _mm256_add_ps(expsum_vec, result_vec);
  }

  float expsum = horizontal_sum(expsum_vec);
  float expsum_reciprocal = 1.0f / expsum;
  __m256 expsum_reciprocal_vec = _mm256_set1_ps(expsum_reciprocal);

  for (size_t i = 0; i < len; i += 8)
  {
    __m256 softmax_probs_vec = _mm256_load_ps(&probs[i]);
    __m256 result_vec = _mm256_mul_ps(softmax_probs_vec, expsum_reciprocal_vec);
    _mm256_store_ps(&probs[i], result_vec);
  }
}

#endif
