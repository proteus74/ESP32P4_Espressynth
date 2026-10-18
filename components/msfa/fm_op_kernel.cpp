/*
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Espressynth: the NEON path and the "#if 0" experimental sine generators of the upstream
// file are dropped; the three kernels are placed in IRAM and use a single mulh per sample.

#include <math.h>

#include <cstdlib>

#include "esp_attr.h"

#include "synth.h"
#include "sin.h"
#include "fm_op_kernel.h"

namespace {
// (a * b) >> 32 - one RV32IM `mulh` instruction.
inline int32_t mulhi32(int32_t a, int32_t b) {
  return (int32_t)(((int64_t)a * (int64_t)b) >> 32);
}

// Upstream computes `((int64_t)y * gain) >> 24` per sample (mul + mulh + shifts on RV32).
// With y a Q24 sine (|y| <= 2^24) and gain < 2^29 the same value is mulhi32(y << 6, gain << 2):
// (y * 2^6 * gain * 2^2) >> 32 == (y * gain) >> 24, and both operands still fit int32
// (gain from Exp2::lookup stays below 2^27 for any DX7 level). gain << 2 is carried in the
// loop as gain4/dgain4, so the per-sample cost is one shift and one mulh.
inline int32_t scaleQ24(int32_t y, int32_t gain4) {
  return mulhi32(y << 6, gain4);
}
}  // namespace

IRAM_ATTR void FmOpKernel::compute(int32_t *output, const int32_t *input,
                                   int32_t phase0, int32_t freq,
                                   int32_t gain1, int32_t gain2, bool add) {
  int32_t dgain = (gain2 - gain1 + (_N_ >> 1)) >> LG_N;
  int32_t gain4 = gain1 << 2;
  const int32_t dgain4 = dgain << 2;
  int32_t phase = phase0;
  if (add) {
    for (int i = 0; i < _N_; i++) {
      gain4 += dgain4;
      int32_t y = Sin::lookup(phase + input[i]);
      output[i] += scaleQ24(y, gain4);
      phase += freq;
    }
  } else {
    for (int i = 0; i < _N_; i++) {
      gain4 += dgain4;
      int32_t y = Sin::lookup(phase + input[i]);
      output[i] = scaleQ24(y, gain4);
      phase += freq;
    }
  }
}

IRAM_ATTR void FmOpKernel::compute_pure(int32_t *output, int32_t phase0, int32_t freq,
                                        int32_t gain1, int32_t gain2, bool add) {
  int32_t dgain = (gain2 - gain1 + (_N_ >> 1)) >> LG_N;
  int32_t gain4 = gain1 << 2;
  const int32_t dgain4 = dgain << 2;
  int32_t phase = phase0;
  if (add) {
    for (int i = 0; i < _N_; i++) {
      gain4 += dgain4;
      int32_t y = Sin::lookup(phase);
      output[i] += scaleQ24(y, gain4);
      phase += freq;
    }
  } else {
    for (int i = 0; i < _N_; i++) {
      gain4 += dgain4;
      int32_t y = Sin::lookup(phase);
      output[i] = scaleQ24(y, gain4);
      phase += freq;
    }
  }
}

IRAM_ATTR void FmOpKernel::compute_fb(int32_t *output, int32_t phase0, int32_t freq,
                                      int32_t gain1, int32_t gain2,
                                      int32_t *fb_buf, int fb_shift, bool add) {
  int32_t dgain = (gain2 - gain1 + (_N_ >> 1)) >> LG_N;
  int32_t gain4 = gain1 << 2;
  const int32_t dgain4 = dgain << 2;
  int32_t phase = phase0;
  int32_t y0 = fb_buf[0];
  int32_t y = fb_buf[1];
  if (add) {
    for (int i = 0; i < _N_; i++) {
      gain4 += dgain4;
      int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
      y0 = y;
      y = scaleQ24(Sin::lookup(phase + scaled_fb), gain4);
      output[i] += y;
      phase += freq;
    }
  } else {
    for (int i = 0; i < _N_; i++) {
      gain4 += dgain4;
      int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
      y0 = y;
      y = scaleQ24(Sin::lookup(phase + scaled_fb), gain4);
      output[i] = y;
      phase += freq;
    }
  }
  fb_buf[0] = y0;
  fb_buf[1] = y;
}
