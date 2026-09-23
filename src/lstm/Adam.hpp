#pragma once

#include "../Array.hpp"
#include "../Utils.hpp"
#include <cstdint>

class Adam
{
protected:
  size_t length;
  float* w;
  float* g;
  Array<float, 32> v;
  float base_lr;

  const float eps = 1e-6f; // ~ 1.0f / 1048576.0f

public:
  Adam(size_t length, float* w, float* g, float base_lr);
  virtual ~Adam() = default;

  virtual void Optimize(float learning_rate, float beta2) = 0;
  virtual void Rescale(float scale) = 0;
};

class Adam_Scalar:
public Adam
{
public:
  Adam_Scalar(size_t length, float* w, float* g, float base_lr):
    Adam(length, w, g, base_lr)
  {}

  virtual void Optimize(float learning_rate, float beta2) override;
  virtual void Rescale(float scale) override;
};

#ifdef X64_SIMD_AVAILABLE

class Adam_SSE2:
public Adam
{
public:
  Adam_SSE2(size_t length, float* w, float* g, float base_lr) :
    Adam(length, w, g, base_lr)
  {}

  virtual void Optimize(float learning_rate, float beta2) override;
  virtual void Rescale(float scale) override;
};
#endif

#ifdef X64_SIMD_AVAILABLE

class Adam_AVX:
public Adam
{
public:
  Adam_AVX(size_t length, float* w, float* g, float base_lr) :
    Adam(length, w, g, base_lr)
  {}

  virtual void Optimize(float learning_rate, float beta2) override;
  virtual void Rescale(float scale) override;
};
#endif
