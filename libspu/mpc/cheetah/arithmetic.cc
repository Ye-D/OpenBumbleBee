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

#include "libspu/mpc/cheetah/arithmetic.h"

#include <future>

#include "libspu/core/ndarray_ref.h"
#include "libspu/core/trace.h"
#include "libspu/mpc/cheetah/arith/common.h"
#include "libspu/mpc/cheetah/nonlinear/compare_prot.h"
#include "libspu/mpc/cheetah/nonlinear/equal_prot.h"
#include "libspu/mpc/cheetah/nonlinear/truncate_prot.h"
#include "libspu/mpc/cheetah/ot/basic_ot_prot.h"
#include "libspu/mpc/cheetah/state.h"
#include "libspu/mpc/cheetah/tiled_dispatch.h"
#include "libspu/mpc/cheetah/type.h"
#include "libspu/mpc/common/communicator.h"
#include "libspu/mpc/common/pv2k.h"
#include "libspu/mpc/utils/ring_ops.h"

namespace spu::mpc::cheetah {

NdArrayRef TruncA::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                        size_t bits, SignType sign) const {
  size_t n = x.numel();
  NdArrayRef out(x.eltype(), x.shape());
  if (n == 0) {
    return out;
  }

  return DispatchUnaryFunc(
      ctx, x,
      [&](const NdArrayRef& input,
          const std::shared_ptr<BasicOTProtocols>& base_ot) {
        TruncateProtocol::Meta meta;
        meta.signed_arith = true;
        meta.sign = sign;
        meta.shift_bits = bits;
        meta.use_heuristic = true;
        TruncateProtocol prot(base_ot);
        return prot.Compute(input, meta);
      });
}

// Math:
//  msb(x0 + x1 mod 2^k) = msb(x0) ^ msb(x1) ^ 1{(x0 + x1) > 2^{k-1} - 1}
//  The carry bit
//     1{(x0 + x1) > 2^{k - 1} - 1} = 1{x0 > 2^{k - 1} - 1 - x1}
//  is computed using a Millionare protocol.
NdArrayRef MsbA2B::proc(KernelEvalContext* ctx, const NdArrayRef& x) const {
  const int64_t numel = x.numel();
  const auto field = ctx->getState<Z2kState>()->getDefaultField();
  const size_t nbits = nbits_ == 0 ? SizeOf(field) * 8 : nbits_;
  const size_t shft = nbits - 1;
  SPU_ENFORCE(nbits <= 8 * SizeOf(field));

  NdArrayRef out(x.eltype(), x.shape());
  if (numel == 0) {
    return out.as(makeType<BShrTy>(field, 1));
  }

  const int rank = ctx->getState<Communicator>()->getRank();

  return DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using u2k = std::make_unsigned<ring2k_t>::type;
    const u2k mask = (static_cast<u2k>(1) << shft) - 1; // mask = 2^{k-1} - 1
    NdArrayRef adjusted = ring_zeros(field, x.shape());
    auto xinp = NdArrayView<const u2k>(x);
    auto xadj = NdArrayView<u2k>(adjusted);

    if (rank == 0) {
      // x0
      pforeach(0, numel, [&](int64_t i) { xadj[i] = xinp[i] & mask; });
    } else {
      // 2^{k - 1} - 1 - x1
      pforeach(0, numel, [&](int64_t i) { xadj[i] = (mask - xinp[i]) & mask; });
    }

    auto carry_bit = DispatchUnaryFunc(
                         ctx, adjusted,
                         [&](const NdArrayRef& input,
                             const std::shared_ptr<BasicOTProtocols>& base_ot) {
                           CompareProtocol prot(base_ot);
                           return prot.Compute(input, /*greater*/ true);
                         })
                         .as(x.eltype());
    // [msb(x)]_B <- [1{x0 + x1 > 2^{k- 1} - 1]_B ^ msb(x_i)
    NdArrayView<u2k> _carry_bit(carry_bit);
    pforeach(0, numel, [&](int64_t i) { _carry_bit[i] ^= (xinp[i] >> shft); });

    return carry_bit.as(makeType<BShrTy>(field, 1));
  });
}

NdArrayRef EqualAP::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                         const NdArrayRef& y) const {
  // NOTE(lwj): this is a temporary dirty hack to reduce the costs of
  // token-id-to-one-hot.
  int iequal_bits = 0;
  const auto* env_str = std::getenv("SPU_BB_SET_IEQUAL_BITS");
  if (env_str != nullptr) {
    char* pEnd;
    auto bits = std::strtol(env_str, &pEnd, 10);
    if (*pEnd == 0) {
      iequal_bits = std::min<int>(x.elsize() * 8, std::max<int>(bits, 0));
    }
  }

  const auto field = ctx->getState<Z2kState>()->getDefaultField();
  EqualAA equal_aa(iequal_bits);

  if (0 == ctx->getState<Communicator>()->getRank()) {
    return equal_aa.proc(ctx, x, ring_zeros(field, x.shape()));
  } else {
    return equal_aa.proc(ctx, x, y);
  }
}

NdArrayRef EqualAA::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                         const NdArrayRef& y) const {
  SPU_ENFORCE_EQ(x.shape(), y.shape());

  const int64_t numel = x.numel();
  const auto field = ctx->getState<Z2kState>()->getDefaultField();
  const size_t nbits = nbits_ == 0 ? SizeOf(field) * 8 : nbits_;
  SPU_ENFORCE(nbits <= 8 * SizeOf(field));

  NdArrayRef eq_bit(x.eltype(), x.shape());
  if (numel == 0) {
    return eq_bit.as(makeType<BShrTy>(field, 1));
  }

  const int rank = ctx->getState<Communicator>()->getRank();

  //     x0 + x1 = y0 + y1 mod 2k
  // <=> x0 - y0 = y1 - x1 mod 2k
  NdArrayRef adjusted;
  if (rank == 0) {
    adjusted = ring_sub(x, y);
  } else {
    adjusted = ring_sub(y, x);
  }

  return DispatchUnaryFunc(
             ctx, adjusted,
             [&](const NdArrayRef& input,
                 const std::shared_ptr<BasicOTProtocols>& base_ot) {
               EqualProtocol prot(base_ot);
               return prot.Compute(input, nbits);
             })
      .as(makeType<BShrTy>(field, 1));
}

NdArrayRef MulA1B::proc(KernelEvalContext* ctx, const NdArrayRef& ashr,
                        const NdArrayRef& bshr) const {
  SPU_ENFORCE_EQ(ashr.shape(), bshr.shape());
  const int64_t numel = ashr.numel();

  if (numel == 0) {
    return NdArrayRef(ashr.eltype(), ashr.shape());
  }

  return DispatchBinaryFunc(
             ctx, ashr, bshr,
             [&](const NdArrayRef& input0, const NdArrayRef& input1,
                 const std::shared_ptr<BasicOTProtocols>& base_ot) {
               return base_ot->Multiplexer(input0, input1);
             })
      .as(ashr.eltype());
}

NdArrayRef MulA1BV::proc(KernelEvalContext* ctx, const NdArrayRef& ashr,
                         const NdArrayRef& bshr) const {
  auto* comm = ctx->getState<Communicator>();
  const int rank = comm->getRank();
  SPU_ENFORCE_EQ(ashr.shape(), bshr.shape());
  const int64_t numel = ashr.numel();
  const auto* ptype = bshr.eltype().as<Priv2kTy>();
  SPU_ENFORCE(ptype != nullptr, "rhs should be a private type");

  const int owner = ptype->owner();

  NdArrayRef out(ashr.eltype(), ashr.shape());
  if (numel == 0) {
    return out;
  }

  if (rank != owner) {
    return DispatchUnaryFunc(
               ctx, ashr,
               [&](const NdArrayRef& input,
                   const std::shared_ptr<BasicOTProtocols>& base_ot) {
                 return base_ot->PrivateMulxSend(input);
               })
        .as(ashr.eltype());
  }

  return DispatchBinaryFunc(
             ctx, ashr, bshr,
             [&](const NdArrayRef& input0, const NdArrayRef& input1,
                 const std::shared_ptr<BasicOTProtocols>& base_ot) {
               return base_ot->PrivateMulxRecv(input0, input1);
             })
      .as(ashr.eltype());
}

NdArrayRef MulAV::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                       const NdArrayRef& y) const {
  SPU_ENFORCE_EQ(x.shape(), y.shape());
  const int64_t numel = x.numel();
  if (numel == 0) {
    return NdArrayRef(x.eltype(), x.shape());
  }
  auto* comm = ctx->getState<Communicator>();
  const int rank = comm->getRank();
  const auto* ptype = y.eltype().as<Priv2kTy>();
  SPU_ENFORCE(ptype != nullptr, "rhs should be a private type");
  const int owner = ptype->owner();

  auto* mul_prot = ctx->getState<CheetahMulState>()->get();
  mul_prot->LazyInitKeys(x.eltype().as<Ring2k>()->field());

  // (x0 + x1) * y
  // <x0 * y> + x1 * y
  auto fx = x.reshape({numel});
  NdArrayRef out;

  // compute <x0 * y>
  if (rank != owner) {
    out = mul_prot->MulOLE(fx, /*eval*/ true);
  } else {// rank == owner
    auto fy = y.reshape({numel});
    out = mul_prot->MulOLE(fy, /*eval*/ false);
    ring_add_(out, ring_mul(fx, fy));
  }

  return out.reshape(x.shape()).as(x.eltype());
}

NdArrayRef MulAA::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                       const NdArrayRef& y) const {
  SPU_ENFORCE_EQ(x.shape(), y.shape());

  int64_t batch_sze = ctx->getState<CheetahMulState>()->get()->OLEBatchSize();
  int64_t numel = x.numel();

  if (numel >= 2 * batch_sze) {
    return mulDirectly(ctx, x, y);
  }
  return mulWithBeaver(ctx, x, y);
}

NdArrayRef SquareA::proc(KernelEvalContext* ctx, const NdArrayRef& x) const {
  const int64_t numel = x.numel();
  if (numel == 0) {
    return NdArrayRef(x.eltype(), x.shape());
  }

  //   (x0 + x1) * (x0 + x1)
  // = x0^2 + 2*<x0*x1> + x1^2
  auto* comm = ctx->getState<Communicator>();
  const int rank = comm->getRank();
  auto* mul_prot = ctx->getState<CheetahMulState>()->get();
  mul_prot->LazyInitKeys(x.eltype().as<Ring2k>()->field());

  auto fx = x.reshape({numel});
  int64_t nhalf = numel <= 8192 ? numel : numel / 2;

  auto subtask = std::async([&]() -> spu::NdArrayRef {
    return mul_prot->MulOLE(fx.slice({0}, {nhalf}, {1}), rank == 0);
  });

  NdArrayRef mul1;
  if (nhalf < numel) {
    auto dupx = ctx->getState<CheetahMulState>()->duplx();
    mul1 = mul_prot->MulOLE(fx.slice({nhalf}, {numel}, {1}), dupx.get(),
                            rank == 1);
  }
  auto mul0 = subtask.get();

  NdArrayRef x0x1(x.eltype(), {numel});
  std::memcpy(&x0x1.at(0), &mul0.at(0), mul0.elsize() * nhalf);
  if (nhalf < numel) {
    std::memcpy(&x0x1.at(nhalf), &mul1.at(0), mul1.elsize() * mul1.numel());
  }
  ring_add_(x0x1, x0x1);
  x0x1 = x0x1.reshape(x.shape());

  return ring_add(x0x1, ring_mul(x, x)).as(x.eltype());
}

NdArrayRef MulAA::mulWithBeaver(KernelEvalContext* ctx, const NdArrayRef& x,
                                const NdArrayRef& y) const {
  const int64_t numel = x.numel();
  if (numel == 0) {
    return NdArrayRef(x.eltype(), x.shape());
  }

  const auto field = ctx->getState<Z2kState>()->getDefaultField();
  auto [a, b, c] =
      ctx->getState<CheetahMulState>()->TakeCachedBeaver(field, numel);
  YACL_ENFORCE_EQ(a.numel(), numel);

  a = a.reshape(x.shape());
  b = b.reshape(x.shape());
  c = c.reshape(x.shape());

  auto* comm = ctx->getState<Communicator>();
  // Open x - a & y - b
  auto res = vmap({ring_sub(x, a), ring_sub(y, b)}, [&](const NdArrayRef& s) {
    return comm->allReduce(ReduceOp::ADD, s, kBindName);
  });
  auto x_a = std::move(res[0]);
  auto y_b = std::move(res[1]);

  // Zi = Ci + (X - A) * Bi + (Y - B) * Ai + <(X - A) * (Y - B)>
  auto z = ring_add(ring_mul(x_a, b), ring_mul(y_b, a));
  ring_add_(z, c);

  if (comm->getRank() == 0) {
    // z += (X-A) * (Y-B);
    ring_add_(z, ring_mul(x_a, y_b));
  }

  return z.as(x.eltype());
}

NdArrayRef MulAA::mulDirectly(KernelEvalContext* ctx, const NdArrayRef& x,
                              const NdArrayRef& y) const {
  // Compute (x0 + x1) * (y0+ y1)
  auto* comm = ctx->getState<Communicator>();
  auto* mul_prot = ctx->getState<CheetahMulState>()->get();
  mul_prot->LazyInitKeys(x.eltype().as<Ring2k>()->field());

  auto fx = x.reshape({x.numel()});
  auto fy = y.reshape({y.numel()});
  const int64_t n = fx.numel();
  const int64_t nhalf = n / 2;
  const int rank = comm->getRank();

  // For long vectors, split into two subtasks.
  auto dupx = ctx->getState<CheetahMulState>()->duplx();
  std::future<NdArrayRef> task = std::async(std::launch::async, [&] {
    return mul_prot->MulShare(fx.slice({nhalf}, {n}, {1}),
                              fy.slice({nhalf}, {n}, {1}), dupx.get(),
                              /*evaluator*/ rank == 0);
  });

  std::vector<NdArrayRef> out_slices(2);
  out_slices[0] =
      mul_prot->MulShare(fx.slice({0}, {nhalf}, {1}),
                         fy.slice({0}, {nhalf}, {1}), /*evaluato*/ rank != 0);
  out_slices[1] = task.get();

  NdArrayRef out(x.eltype(), x.shape());
  int64_t offset = 0;
  for (auto& out_slice : out_slices) {
    std::memcpy(out.data<std::byte>() + offset, out_slice.data(),
                out_slice.numel() * out.elsize());
    offset += out_slice.numel() * out.elsize();
  }
  return out;
}

NdArrayRef MatMulVVS::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                           const NdArrayRef& y) const {
  auto out_type = makeType<cheetah::AShrTy>(ctx->sctx()->getField());
  if (0 == x.numel() || 0 == y.numel()) {
    return NdArrayRef(out_type, {x.shape()[0], y.shape()[1]});
  }
  auto* comm = ctx->getState<Communicator>();
  auto* dot_prot = ctx->getState<CheetahDotState>()->get();

  const int self_rank = comm->getRank();
  auto lhs_owner = x.eltype().as<Priv2kTy>()->owner();

  const Shape3D dim3 = {x.shape()[0], x.shape()[1], y.shape()[1]};
  if (self_rank == lhs_owner) {
    return dot_prot->DotOLE(x, dim3, /*is_lhs*/ true).as(out_type);
  } else {
    return dot_prot->DotOLE(y, dim3, /*is_lhs*/ false).as(out_type);
  }
}

// A is (M, K); B is (K, N)
NdArrayRef MatMulAA::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                          const NdArrayRef& y) const {
  if (0 == x.numel() || 0 == y.numel()) {
    return NdArrayRef(x.eltype(), {x.shape()[0], y.shape()[1]});
  }

  auto* comm = ctx->getState<Communicator>();
  auto* dot_prot = ctx->getState<CheetahDotState>()->get();
  dot_prot->LazyInitKeys(x.eltype().as<Ring2k>()->field());

  const int rank = comm->getRank();

  // (x0 + x1) * (y0 + y1)
  // Compute the cross terms homomorphically
  const Shape3D dim3 = {x.shape()[0], x.shape()[1], y.shape()[1]};

  auto* conn = comm->lctx().get();
  auto dupx = ctx->getState<CheetahMulState>()->duplx();
  std::future<NdArrayRef> task = std::async(std::launch::async, [&] {
    // Compute x0*y1
    if (rank == 0) {
      return dot_prot->DotOLE(x, dupx.get(), dim3, true);
    } else {
      return dot_prot->DotOLE(y, dupx.get(), dim3, false);
    }
  });

  NdArrayRef x1y0;
  if (rank == 0) {
    x1y0 = dot_prot->DotOLE(y, conn, dim3, false);
  } else {
    x1y0 = dot_prot->DotOLE(x, conn, dim3, true);
  }

  auto ret = ring_mmul(x, y);
  ring_add_(ret, x1y0);
  return ring_add(ret, task.get()).as(x.eltype());
}

NdArrayRef MatMulAV::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                          const NdArrayRef& y) const {
  if (0 == x.numel() || 0 == y.numel()) {
    return NdArrayRef(x.eltype(), {x.shape()[0], y.shape()[1]});
  }
  auto* comm = ctx->getState<Communicator>();
  auto* dot_prot = ctx->getState<CheetahDotState>()->get();
  dot_prot->LazyInitKeys(x.eltype().as<Ring2k>()->field());

  const int rank = comm->getRank();
  const auto* ptype = y.eltype().as<Priv2kTy>();
  SPU_ENFORCE(ptype != nullptr, "rhs should be a private type");
  const int owner = ptype->owner();
  NdArrayRef out;
  const Shape3D dim3 = {x.shape()[0], x.shape()[1], y.shape()[1]};
  // (x0 + x1)*y = <x0 * y>_0 + <x0 * y>_1 + x1 * y
  if (rank == owner) {
    // Compute <y * x0>
    out = dot_prot->DotOLE(y, dim3, false);
    auto local = ring_mmul(x, y);
    ring_add_(out, local);
  } else {
    out = dot_prot->DotOLE(x, dim3, true);
  }
  return out.as(x.eltype());
}

void BatchMatMulAV::evaluate(KernelEvalContext* ctx) const {
  const auto& lhs = ctx->getParam<Value>(0);
  const auto& rhs = ctx->getParam<Value>(1);
  auto xs = lhs.shape();
  auto ys = rhs.shape();
  SPU_ENFORCE(xs.ndim() == ys.ndim(), "ndim mismatch: lhs={}, rhs={}", xs, ys);
  SPU_ENFORCE(xs[0] == ys[0], "batch mismatch: lhs={}, rhs={}", xs, ys);
  SPU_ENFORCE(xs[2] == ys[1], "shape mismatch: lhs={}, rhs={}", xs, ys);
  ctx->pushOutput(WrapValue(proc(ctx, lhs.data(), rhs.data())));
}

// A is (B, M, K); B is (B, K, N)
NdArrayRef BatchMatMulAV::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                               const NdArrayRef& y) const {
  if (0 == x.numel() || 0 == y.numel()) {
    return NdArrayRef(x.eltype(), {x.shape()[0], y.shape()[1]});
  }
  SPU_ENFORCE(x.ndim() == 3 && y.ndim() == 3);
  SPU_ENFORCE_EQ(x.shape()[0], y.shape()[0]);
  SPU_ENFORCE_EQ(x.shape()[2], y.shape()[1]);

  auto* comm = ctx->getState<Communicator>();
  auto* dot_prot = ctx->getState<CheetahDotState>()->get();
  const int rank = comm->getRank();
  const auto* ptype = y.eltype().as<Priv2kTy>();
  SPU_ENFORCE(ptype != nullptr, "rhs should be a private type");
  const int owner = ptype->owner();

  // (x0 + x1)*y = <x0 * y>_0 + <x0 * y>_1 + x1 * y
  const Shape4D dim4 = {x.shape()[0], x.shape()[1], x.shape()[2], y.shape()[2]};

  NdArrayRef out;
  if (rank != owner) {
    out = dot_prot->BatchDotOLE(x, comm->lctx().get(), dim4, true);
  } else {
    out = dot_prot->BatchDotOLE(y, comm->lctx().get(), dim4, false);

    const Strides strides(x.shape().size(), 1);
    Index lhs_slice_end(x.shape().begin(), x.shape().end());
    Index rhs_slice_end(y.shape().begin(), y.shape().end());
    Index lhs_slice_begin(3, 0);
    Index rhs_slice_begin(3, 0);

    for (int64_t b = 0; b < dim4[0]; ++b) {
      lhs_slice_begin[0] = b;
      lhs_slice_end[0] = b + 1;
      rhs_slice_begin[0] = b;
      rhs_slice_end[0] = b + 1;
      auto lhs = x.slice(lhs_slice_begin, lhs_slice_end, strides)
                     .reshape({dim4[1], dim4[2]});
      auto rhs = y.slice(rhs_slice_begin, rhs_slice_end, strides)
                     .reshape({dim4[2], dim4[3]});
      auto local = ring_mmul(lhs, rhs);

      auto out_slice = out.slice({b, 0, 0}, {b + 1, dim4[1], dim4[3]}, strides);
      out_slice = out_slice.reshape({dim4[1], dim4[3]});
      ring_add_(out_slice, local);
    }
  }

  return out.as(x.eltype());
}

void BatchMatMulAA::evaluate(KernelEvalContext* ctx) const {
  const auto& lhs = ctx->getParam<Value>(0);
  const auto& rhs = ctx->getParam<Value>(1);
  auto xs = lhs.shape();
  auto ys = rhs.shape();
  SPU_ENFORCE(xs.ndim() == ys.ndim(), "ndim mismatch: lhs={}, rhs={}", xs, ys);
  SPU_ENFORCE(xs[0] == ys[0], "batch mismatch: lhs={}, rhs={}", xs, ys);
  SPU_ENFORCE(xs[2] == ys[1], "shape mismatch: lhs={}, rhs={}", xs, ys);
  ctx->pushOutput(WrapValue(proc(ctx, lhs.data(), rhs.data())));
}

// A is (B, M, K); B is (B, K, N)
NdArrayRef BatchMatMulAA::proc(KernelEvalContext* ctx, const NdArrayRef& x,
                               const NdArrayRef& y) const {
  if (0 == x.numel() || 0 == y.numel()) {
    return NdArrayRef(x.eltype(), {x.shape()[0], y.shape()[1]});
  }
  SPU_ENFORCE(x.ndim() == 3 && y.ndim() == 3);
  SPU_ENFORCE_EQ(x.shape()[0], y.shape()[0]);
  SPU_ENFORCE_EQ(x.shape()[2], y.shape()[1]);

  auto* comm = ctx->getState<Communicator>();
  auto* dot_prot = ctx->getState<CheetahDotState>()->get();
  const int rank = comm->getRank();
  dot_prot->LazyInitKeys(x.eltype().as<Ring2k>()->field());

  // (x0 + x1) * (y0 + y1)
  // Compute the cross terms
  const Shape4D dim4 = {x.shape()[0], x.shape()[1], x.shape()[2], y.shape()[2]};

  auto* conn = comm->lctx().get();
  auto dupx = ctx->getState<CheetahMulState>()->duplx();
  std::future<NdArrayRef> task = std::async(std::launch::async, [&] {
    // Compute x0*y1
    if (rank == 0) {
      return dot_prot->BatchDotOLE(x, dupx.get(), dim4, true);
    } else {
      return dot_prot->BatchDotOLE(y, dupx.get(), dim4, false);
    }
  });

  NdArrayRef x1y0;
  if (rank == 0) {
    x1y0 = dot_prot->BatchDotOLE(y, conn, dim4, false);
  } else {
    x1y0 = dot_prot->BatchDotOLE(x, conn, dim4, true);
  }

  const Strides strides(x.shape().size(), 1);
  Index lhs_slice_end(x.shape().begin(), x.shape().end());
  Index rhs_slice_end(y.shape().begin(), y.shape().end());
  Index lhs_slice_begin(3, 0);
  Index rhs_slice_begin(3, 0);
  NdArrayRef out(x.eltype(), {dim4[0], dim4[1], dim4[3]});
  for (int64_t b = 0; b < dim4[0]; ++b) {
    lhs_slice_begin[0] = b;
    lhs_slice_end[0] = b + 1;
    rhs_slice_begin[0] = b;
    rhs_slice_end[0] = b + 1;
    auto lhs = x.slice(lhs_slice_begin, lhs_slice_end, strides)
                   .reshape({dim4[1], dim4[2]});
    auto rhs = y.slice(rhs_slice_begin, rhs_slice_end, strides)
                   .reshape({dim4[2], dim4[3]});

    auto out_slice = out.slice({b, 0, 0}, {b + 1, dim4[1], dim4[3]}, strides);
    out_slice = out_slice.reshape({dim4[1], dim4[3]});
    ring_mmul_(out_slice, lhs, rhs);
  }

  ring_add_(out, x1y0);
  ring_add_(out, task.get());
  return out.as(x.eltype());
}
}  // namespace spu::mpc::cheetah
