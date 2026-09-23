#include "Adam.hpp"
#include "../Array.hpp"
#include <cmath>
#include <cstddef>

Adam::Adam(size_t length, float* w, float* g, float base_lr)
  : length(length)
  , w(w)
  , g(g)
  , v(length)
  , base_lr(base_lr)
{}

void Adam_Scalar::Optimize(float lr_rate, float beta2)
{
  float const lr = base_lr * lr_rate;

  for (size_t i = 0; i < length; i++)
  {
    float g_val = g[i];
    float v_old = v[i];
    float g_sq = g_val * g_val;
    float v_new = v_old * beta2 + (1.0f - beta2) * g_sq;
    v[i] = v_new;
    float denom = std::sqrt(v_new) + eps;
    float scaled_gradient = g_val / denom;
    g[i] = 0.0f;
    w[i] = w[i] - lr * scaled_gradient;
  }
}

void Adam_Scalar::Rescale(float scale)
{
  for (size_t i = 0; i < length; i++)
  {
    v[i] = v[i] * scale;
  }
}

#ifdef X64_SIMD_AVAILABLE

#if (defined(__GNUC__) || defined(__clang__))
#pragma GCC target("sse2")
#endif

void Adam_SSE2::Optimize(float lr_scale, float beta2)
{
  __m128 const vec_zero = _mm_setzero_ps();
  __m128 const vec_beta2 = _mm_set1_ps(beta2);
  __m128 const vec_eps = _mm_set1_ps(eps);
  __m128 const vec_beta2_complement = _mm_set1_ps(1.f - beta2);

  __m128 const vec_lr = _mm_set1_ps(base_lr * lr_scale);

  for (size_t i = 0; i < length; i += 4) {
    __m128 vec_gi = _mm_load_ps(&g[i]);
    __m128 vec_vi = _mm_load_ps(&v[i]);

    // v = beta2 * v + (1 - beta2) * g^2
    vec_vi = _mm_mul_ps(vec_vi, vec_beta2);
    __m128 vec_gi_sq = _mm_mul_ps(vec_gi, vec_gi);
    __m128 vec_term = _mm_mul_ps(vec_gi_sq, vec_beta2_complement);
    vec_vi = _mm_add_ps(vec_vi, vec_term);
    _mm_store_ps(&v[i], vec_vi);

    // scaled_gradient = g / (sqrt(v) + eps)
    __m128 vec_sqrt = _mm_sqrt_ps(vec_vi);
    __m128 vec_denom = _mm_add_ps(vec_sqrt, vec_eps);
    __m128 vec_scaled_grad = _mm_div_ps(vec_gi, vec_denom);
    _mm_store_ps(&g[i], vec_zero);

    // w = w - lr * scaled_gradient
    __m128 vec_wi = _mm_load_ps(&w[i]);
    __m128 vec_update = _mm_mul_ps(vec_lr, vec_scaled_grad);
    vec_wi = _mm_sub_ps(vec_wi, vec_update);
    _mm_store_ps(&w[i], vec_wi);
  }
}

void Adam_SSE2::Rescale(float scale)
{
  __m128 const vec_scale = _mm_set1_ps(scale);

  for (size_t i = 0; i < length; i += 4)
  {
    __m128 vec_vi = _mm_load_ps(&v[i]);
    vec_vi = _mm_mul_ps(vec_vi, vec_scale);
    _mm_store_ps(&v[i], vec_vi);
  }
}

#endif

#ifdef X64_SIMD_AVAILABLE

#if (defined(__GNUC__) || defined(__clang__))
#define AVX2_TARGET __attribute__((target("avx2")))
#else
#define AVX2_TARGET
#endif

AVX2_TARGET
void Adam_AVX::Optimize(float lr_scale, float beta2)
{
  __m256 const vec_zero = _mm256_setzero_ps();
  __m256 const vec_beta2 = _mm256_set1_ps(beta2);
  __m256 const vec_eps = _mm256_set1_ps(eps);
  __m256 const vec_beta2_complement = _mm256_set1_ps(1.f - beta2);

  __m256 const vec_lr = _mm256_set1_ps(base_lr * lr_scale);

  for (size_t i = 0; i < length; i += 8)
  {
    __m256 vec_gi = _mm256_load_ps(&g[i]);
    __m256 vec_vi = _mm256_load_ps(&v[i]);

    // v = beta2 * v + (1 - beta2) * g^2
    vec_vi = _mm256_mul_ps(vec_vi, vec_beta2);
    __m256 vec_gi_sq = _mm256_mul_ps(vec_gi, vec_gi);
    __m256 vec_term = _mm256_mul_ps(vec_gi_sq, vec_beta2_complement);
    vec_vi = _mm256_add_ps(vec_vi, vec_term);
    _mm256_store_ps(&v[i], vec_vi);

    // scaled_gradient = g / (sqrt(v) + eps)
    __m256 vec_sqrt = _mm256_sqrt_ps(vec_vi);
    __m256 vec_denom = _mm256_add_ps(vec_sqrt, vec_eps);
    __m256 vec_scaled_grad = _mm256_div_ps(vec_gi, vec_denom);
    _mm256_store_ps(&g[i], vec_zero);

    // w = w - lr * scaled_gradient
    __m256 vec_wi = _mm256_load_ps(&w[i]);
    __m256 vec_update = _mm256_mul_ps(vec_lr, vec_scaled_grad);
    vec_wi = _mm256_sub_ps(vec_wi, vec_update);
    _mm256_store_ps(&w[i], vec_wi);
  }
}

AVX2_TARGET
void Adam_AVX::Rescale(float scale)
{
  __m256 const vec_scale = _mm256_set1_ps(scale);

  for (size_t i = 0; i < length; i += 8) {
    __m256 vec_vi = _mm256_load_ps(&v[i]);
    vec_vi = _mm256_mul_ps(vec_vi, vec_scale);
    _mm256_store_ps(&v[i], vec_vi);
  }
}

#endif
