// Copyright 2021 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "libspu/core/value.h"

namespace spu {
class SPUContext;
}

// !!please read [README.md] for api naming conventions.
namespace spu::kernel::hal {
namespace detail {

// Extract the most significant bit. see
// https://docs.oracle.com/javase/7/docs/api/java/lang/Integer.html#highestOneBit(int)
Value highestOneBit(SPUContext* ctx, const Value& x);

void hintNumberOfBits(const Value& a, size_t nbits);

// we provide this general function to support some special cases (a or b has
// guarranteed sign) in fxp_approx for better both performance and accuracy.
Value div_goldschmidt_general(SPUContext* ctx, const Value& a, const Value& b,
                              SignType a_sign = SignType::Unknown,
                              SignType b_sign = SignType::Unknown);

Value div_goldschmidt(SPUContext* ctx, const Value& a, const Value& b);

Value reciprocal_goldschmidt_positive(SPUContext* ctx, const Value& b_abs);

Value reciprocal_goldschmidt(SPUContext* ctx, const Value& b);

Value polynomial(SPUContext* ctx, const Value& x,
                 absl::Span<Value const> coeffs,
                 SignType sign_x = SignType::Unknown,
                 SignType sign_ret = SignType::Unknown);

Value polynomial(SPUContext* ctx, const Value& x,
                 absl::Span<float const> coeffs,
                 SignType sign_x = SignType::Unknown,
                 SignType sign_ret = SignType::Unknown);

}  // namespace detail

Value f_negate(SPUContext* ctx, const Value& x);

Value f_abs(SPUContext* ctx, const Value& x);

Value f_reciprocal(SPUContext* ctx, const Value& x);

Value f_add(SPUContext* ctx, const Value& x, const Value& y);

Value f_sub(SPUContext* ctx, const Value& x, const Value& y);

Value f_mul(SPUContext* ctx, const Value& x, const Value& y,
            SignType sign = SignType::Unknown);

Value f_mmul(SPUContext* ctx, const Value& x, const Value& y);

std::optional<Value> f_batch_mmul(SPUContext* ctx, const Value& x,
                                  const Value& y);

Value f_conv2d(SPUContext* ctx, const Value& x, const Value& y,
               const Strides& window_strides);

Value f_tensordot(SPUContext* ctx, const Value& x, const Value& y,
                  const Index& ix, const Index& iy);

Value f_div(SPUContext* ctx, const Value& x, const Value& y);

Value f_equal(SPUContext* ctx, const Value& x, const Value& y);

Value f_less(SPUContext* ctx, const Value& x, const Value& y);

Value f_square(SPUContext* ctx, const Value& x);

Value f_floor(SPUContext* ctx, const Value& x);

Value f_ceil(SPUContext* ctx, const Value& x);

}  // namespace spu::kernel::hal
