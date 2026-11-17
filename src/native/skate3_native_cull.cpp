// Sk8 vectorized frustum culling, native (see the header and
// docs/re/culling.md).
//
// Vectors are kept the way the recompiled code keeps them (lvx = a 16-byte
// load byte-reversed, so host lane 3 is the guest's element 0), and every
// float operation is the recompiler's primitive for the guest instruction
// (vmaddfp = fused multiply-add, vrefp = 1/x, vminfp = minps operand
// order), in the guest's order. That is what makes the result bit-exact;
// the names say what each value is.

#include "native/skate3_native_cull.h"

#include <atomic>
#include <cstddef>
#include <bit>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

#include "generated/skate3_init.h"
#include "native/skate3_native_guest_frame.h"
#include "native/skate3_native_replace.h"

REXCVAR_DEFINE_STRING(skate3_native_cull_replace, "on", "Skate 3",
                      "RE-7: Sk8 frustum culling (DoFrustumCulling sub_82816A40 and its job "
                      "wrappers): off | shadow (native computes, compared with the recompiled "
                      "code; replace: cull) | on")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_cull {
namespace {

namespace nr = skate3::native_replace;

std::atomic<int> g_mode{-1};

int ReadMode() {
  const std::string v = REXCVAR_GET(skate3_native_cull_replace);
  return v == "on" ? 2 : v == "shadow" ? 1 : 0;
}

int Mode() {
  int m = g_mode.load(std::memory_order_relaxed);
  if (m < 0) {
    m = ReadMode();
    g_mode.store(m, std::memory_order_relaxed);
  }
  return m;
}

using V = simde__m128;

// A guest address in host memory, as REX_RAW_ADDR maps it (Windows keeps the
// physical mirror at 0xE0000000+ 0x1000 higher).
inline uint8_t* Raw(uint8_t* base, uint32_t addr) {
#if defined(_WIN32)
  return base + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + addr;
#endif
}

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t r;
  std::memcpy(&r, Raw(base, addr), 4);
  return __builtin_bswap32(r);
}

inline void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Raw(base, addr), &v, 4);
}

inline bool Readable(uint32_t addr, uint32_t n) {
  return addr >= 0x10000 && uint64_t(addr) + n <= 0x100000000ull;
}

// lvx: 16 bytes at addr & ~15, byte-reversed into host order.
inline V Load(uint8_t* base, uint32_t addr) {
  const simde__m128i raw =
      simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(Raw(base, addr & ~15u)));
  return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(
      raw, simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(rex::ppc::VectorMaskL))));
}

// stvx's byte image of v (what the guest's 16 bytes would hold).
inline void Image(V v, uint8_t out[16]) {
  const simde__m128i b = simde_mm_shuffle_epi8(
      simde_mm_castps_si128(v),
      simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(rex::ppc::VectorMaskL)));
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(out), b);
}

// vspltw element e (0 = the guest's first element).
template <int e>
inline V Splat(V v) {
  constexpr int imm = e == 0 ? 0xFF : e == 1 ? 0xAA : e == 2 ? 0x55 : 0x00;
  return simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(v), imm));
}

// vmrghw / vmrglw a, b.
inline V MergeHigh(V a, V b) {
  return simde_mm_castsi128_ps(
      simde_mm_unpackhi_epi32(simde_mm_castps_si128(b), simde_mm_castps_si128(a)));
}
inline V MergeLow(V a, V b) {
  return simde_mm_castsi128_ps(
      simde_mm_unpacklo_epi32(simde_mm_castps_si128(b), simde_mm_castps_si128(a)));
}

inline V Fma(V a, V b, V c) { return simde_mm_fmadd_ps(a, b, c); }      // a*b + c, fused
inline V Fnma(V a, V b, V c) { return simde_mm_fnmadd_ps(a, b, c); }    // c - a*b, fused
inline V Mul(V a, V b) { return simde_mm_mul_ps(a, b); }
inline V Add(V a, V b) { return simde_mm_add_ps(a, b); }
inline V Sub(V a, V b) { return simde_mm_sub_ps(a, b); }
inline V Min(V a, V b) { return simde_mm_min_ps(a, b); }
inline V Max(V a, V b) { return simde_mm_max_ps(a, b); }
inline V Ge(V a, V b) { return simde_mm_cmpge_ps(a, b); }               // a >= b
inline V And(V a, V b) { return simde_mm_and_ps(a, b); }
inline V Xor(V a, V b) { return simde_mm_xor_ps(a, b); }
// vsel: mask bits from b, others from a.
inline V Select(V a, V b, V mask) {
  return simde_mm_or_ps(simde_mm_andnot_ps(mask, a), simde_mm_and_ps(mask, b));
}
// vandc x, signmask: |x|.
inline V Abs(V v) {
  return simde_mm_andnot_ps(simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000u))), v);
}
inline bool NoneSet(V mask) {
  return simde_mm_movemask_epi8(
             simde_mm_cmpeq_epi32(simde_mm_castps_si128(mask), simde_mm_setzero_si128())) ==
         0xFFFF;
}
inline V Recip(V v) { return simde_mm_div_ps(simde_mm_set1_ps(1.0f), v); }  // vrefp

// One plane: splatted normal components and the splatted distance.
struct Plane {
  V x, y, z, d;    // as stored: (x, y, z, d)
  V ax, ay, az;    // |x|, |y|, |z|
};

Plane LoadPlane(uint8_t* base, uint32_t addr) {
  const V p = Load(base, addr);
  Plane q;
  q.x = Splat<0>(p);
  q.y = Splat<1>(p);
  q.z = Splat<2>(p);
  q.d = Splat<3>(p);
  q.ax = Abs(q.x);
  q.ay = Abs(q.y);
  q.az = Abs(q.z);
  return q;
}

using rex::be;

// One occluder plane as the culling data stores it: a point on the plane and
// its normal. An occluder is 5 of them.
struct OccluderPlane {
  be<float> point[4];
  be<float> normal[4];
};
static_assert(sizeof(OccluderPlane) == 32);
constexpr uint32_t kPlanesPerOccluder = 5;

// The culling parameters (VectorizedCullingJob's data, *(job + 8)).
struct CullingParams {
  OccluderPlane occluder_planes[160];  // +0      kPlanesPerOccluder per occluder
  uint8_t unknown_1400[16];
  be<float> size_axis[4];              // +5136   (x, y, z, -): |axis| weights an item's size vector
  be<float> frustum[6][4];             // +5152   planes (x, y, z, d)
  uint8_t unknown_1480[12];
  be<uint32_t> occluder_count;         // +5260
  be<float> size_min;                  // +5264   projected-size threshold
  be<float> size_far;                  // +5268   the size test applies from this depth on
  be<float> size_scale;                // +5272   depth scale (its reciprocal is used)
};
static_assert(offsetof(CullingParams, size_axis) == 5136);
static_assert(offsetof(CullingParams, frustum) == 5152);
static_assert(offsetof(CullingParams, occluder_count) == 5260);
static_assert(offsetof(CullingParams, size_scale) == 5272);
// The frustum plane that also gives the depth for the size test.
constexpr int kDepthPlane = 4;

// An item's bounds.
struct Box {
  be<float> min[4];
  be<float> max[4];
};
static_assert(sizeof(Box) == 32);

// What DoFrustumCulling culls: count items, their boxes, optional size
// vectors, and where each item's u16 visibility mask goes.
struct CullDescriptor {
  be<uint32_t> boxes;       // Box[count]
  be<uint32_t> sizes;       // be<float>[4][count], or 0
  be<uint32_t> unknown_08;
  be<uint32_t> visibility;  // be<u16>[count]
  be<uint16_t> count;       // +16
};
static_assert(offsetof(CullDescriptor, count) == 16);

// An occluder plane splatted for the vector test (built on the job's stack):
// px, py, pz, nx, ny, nz, each a 16-byte vector of one repeated value.
constexpr uint32_t kSplattedPlaneSize = 96;

template <typename T>
const T& At(uint8_t* base, uint32_t addr) {
  return *reinterpret_cast<const T*>(Raw(base, addr));
}

// DoFrustumCulling(desc r3, params r4, occluder planes r5 (splatted, 96
// bytes per plane, 5 planes per occluder), occluder count r6, change
// counter r7 (u32 x4)).
bool ComputeCull(PPCContext& ctx, uint8_t* base, nr::Writes* w) {
  const uint32_t desc_addr = ctx.r3.u32, params_addr = ctx.r4.u32, occ = ctx.r5.u32;
  const uint32_t occ_count = ctx.r6.u32, counter_addr = ctx.r7.u32;
  if (!Readable(desc_addr, sizeof(CullDescriptor)) ||
      !Readable(params_addr, sizeof(CullingParams))) {
    return false;
  }
  w->Reset(2);  // rows of 8 bytes: one group's 4 u16 masks (the counter: 2 rows)
  ctx.fpscr.enableFlushMode();

  const auto& desc = At<CullDescriptor>(base, desc_addr);
  const auto& params = At<CullingParams>(base, params_addr);
  const uint32_t count = desc.count;
  uint32_t boxes = desc.boxes;
  uint32_t out = desc.visibility;
  uint32_t sizes = desc.sizes;

  const V zero = simde_mm_setzero_ps();
  const V ones = simde_mm_castsi128_ps(simde_mm_set1_epi32(-1));
  const V half = simde_mm_set1_ps(0.5f);
  const V one = simde_mm_set1_ps(1.0f);

  // The size axis and the 6 frustum planes.
  const V size_axis = Load(base, params_addr + offsetof(CullingParams, size_axis));
  const V size_ax = Abs(Splat<0>(size_axis)), size_ay = Abs(Splat<1>(size_axis)),
          size_az = Abs(Splat<2>(size_axis));
  Plane fp[6];
  for (int k = 0; k < 6; ++k) {
    fp[k] = LoadPlane(base, params_addr + offsetof(CullingParams, frustum) + 16 * k);
  }
  V fd[6];  // 1.0 * d (as the guest scales it)
  for (int k = 0; k < 6; ++k) fd[k] = Mul(one, fp[k].d);
  const Plane& nearp = fp[kDepthPlane];

  // Scalars, splatted. The reciprocal of the depth scale with one Newton step.
  const V size_min = simde_mm_set1_ps(params.size_min);
  const V size_far = simde_mm_set1_ps(params.size_far);
  const V scale = simde_mm_set1_ps(params.size_scale);
  V rscale = Recip(scale);
  rscale = Fma(rscale, Fnma(rscale, scale, one), rscale);

  V counter = zero;
  bool counter_used = false;
  if (Readable(counter_addr, 16)) counter = Load(base, counter_addr);

  if (static_cast<int32_t>(count) <= 0) return true;
  const uint32_t groups = ((count - 1) >> 2) + 1;
  // Items go in groups of 4: 4 boxes, 4 size vectors, 4 masks.
  if (!Readable(boxes, groups * 4 * sizeof(Box)) || !Readable(out, groups * 8)) return false;
  for (uint32_t g = 0; g < groups; ++g, boxes += 4 * sizeof(Box), out += 8) {
    const uint32_t sizes_now = desc.sizes;  // re-read per group, as the guest does
    V size_x = zero, size_y = zero, size_z = zero, size_w = zero;
    if (sizes_now != 0) {
      const V e0 = Load(base, sizes), e1 = Load(base, sizes + 16), e2 = Load(base, sizes + 32),
              e3 = Load(base, sizes + 48);
      sizes += 64;
      const V h02 = MergeHigh(e0, e2), h13 = MergeHigh(e1, e3);
      const V l02 = MergeLow(e0, e2), l13 = MergeLow(e1, e3);
      size_x = MergeHigh(h02, h13);
      size_y = MergeLow(h02, h13);
      size_z = MergeHigh(l02, l13);
      size_w = MergeLow(l02, l13);
    }

    // 4 AABBs (min = box 2i, max = box 2i+1), transposed to x/y/z lanes.
    V b[8];
    for (int i = 0; i < 8; ++i) b[i] = Load(base, boxes + 16 * i);
    const V l04 = MergeLow(b[0], b[4]), l26 = MergeLow(b[2], b[6]);
    const V l15 = MergeLow(b[1], b[5]), l37 = MergeLow(b[3], b[7]);
    const V h04 = MergeHigh(b[0], b[4]), h26 = MergeHigh(b[2], b[6]);
    const V h15 = MergeHigh(b[1], b[5]), h37 = MergeHigh(b[3], b[7]);
    const V min_z = MergeHigh(l04, l26), max_z = MergeHigh(l15, l37);
    const V min_y = MergeLow(h04, h26), min_x = MergeHigh(h04, h26);
    const V max_y = MergeLow(h15, h37), max_x = MergeHigh(h15, h37);

    const V cz = Mul(Add(min_z, max_z), half);
    const V cy = Mul(Add(min_y, max_y), half);
    const V cx = Mul(Add(min_x, max_x), half);
    const V ez = Sub(max_z, cz);
    const V ey = Sub(max_y, cy);
    const V ex = Sub(max_x, cx);

    // Each plane: distance of the center (z, then y, then x terms) plus the
    // box's projected radius (|n| . extent, same order).
    V s[6];
    for (int k = 0; k < 6; ++k) {
      const Plane& p = fp[k];
      const V dist = Fma(cx, p.x, Fma(cy, p.y, Fma(cz, p.z, fd[k])));
      const V radius = Fma(ex, p.ax, Fma(ey, p.ay, Mul(ez, p.az)));
      s[k] = Add(radius, dist);
    }
    const V m01 = Min(s[0], s[1]), m23 = Min(s[2], s[3]), m45 = Min(s[4], s[5]);
    V visible = Ge(Min(Min(m01, m23), m45), zero);

    if (!NoneSet(visible) && occ_count != 0) {
      const V after_frustum = visible;
      // Occluders: an item is hidden when all 8 corners are behind (<= 0)
      // all 5 of an occluder's planes.
      uint32_t plane = occ;
      for (uint32_t o = 0; o < occ_count; ++o) {
        V inside = ones;
        for (uint32_t p = 0; p < kPlanesPerOccluder; ++p, plane += kSplattedPlaneSize) {
          const V px = Load(base, plane), py = Load(base, plane + 16), pz = Load(base, plane + 32);
          const V nx = Load(base, plane + 48), ny = Load(base, plane + 64),
                  nz = Load(base, plane + 80);
          const V xs[2] = {max_x, min_x}, ys[2] = {max_y, min_y}, zs[2] = {max_z, min_z};
          for (int c = 0; c < 8; ++c) {
            const V d = Fma(Sub(xs[c & 1], px), nx,
                            Fma(Sub(ys[(c >> 1) & 1], py), ny, Mul(Sub(zs[c >> 2], pz), nz)));
            inside = And(inside, Ge(zero, d));
          }
        }
        visible = And(visible, Xor(inside, ones));
        if (NoneSet(visible)) break;
      }

      // Projected size: from the near-plane depth of the nearest corner on,
      // items whose size / (depth^2 / scale) is below the threshold go.
      if (sizes_now != 0 && !NoneSet(visible) && !NoneSet(size_w)) {
        const V cxn = Select(max_x, min_x, Ge(nearp.x, zero));
        const V cyn = Select(max_y, min_y, Ge(nearp.y, zero));
        const V czn = Select(max_z, min_z, Ge(nearp.z, zero));
        const V size = Fma(size_x, size_ax, Fma(size_y, size_ay, Mul(size_z, size_az)));
        const V depth =
            Max(Add(Fma(cxn, nearp.x, Fma(cyn, nearp.y, Mul(czn, nearp.z))), nearp.d), one);
        const V q = Mul(depth, Mul(depth, rscale));
        V inv = Recip(q);
        inv = Fma(inv, Fnma(inv, q, one), inv);
        const V big_enough = Ge(Mul(size, inv), size_min);
        const V far_away = Ge(depth, size_far);
        const V keep = Select(visible, big_enough, far_away);
        visible = Select(zero, visible, keep);
      }

      // Lanes the occluders or the size test changed.
      counter = simde_mm_castsi128_ps(simde_mm_add_epi32(
          simde_mm_castps_si128(counter),
          simde_mm_and_si128(simde_mm_castps_si128(Xor(after_frustum, visible)),
                             simde_mm_set1_epi32(1))));
      counter_used = true;
    }

    // Each item's mask: the low half of its lane (sth of each word).
    uint8_t img[16];
    Image(visible, img);
    uint32_t* row = w->Add(out);
    uint8_t bytes[8] = {img[2], img[3], img[6], img[7], img[10], img[11], img[14], img[15]};
    std::memcpy(row, bytes, 8);
  }
  if (counter_used) {
    uint8_t img[16];
    Image(counter, img);
    std::memcpy(w->Add(counter_addr & ~15u), img, 8);
    std::memcpy(w->Add((counter_addr & ~15u) + 8), img + 8, 8);
  }
  return true;
}

nr::Family g_cull{"cull", &Mode, &ComputeCull};

// --- The job wrappers (Body families; on when the cull family is on) -------

int BodyMode() { return Mode() == 2 ? 2 : 0; }

using skate3::native_guest::GuestFrame;

// The culling job (VectorizedCullingJob's second argument): its parameters
// at +8.
struct CullJob {
  uint8_t unknown_00[8];
  be<uint32_t> params;  // CullingParams*
};

// VectorizedCullingJob's stack frame (15488 bytes): the change counter
// (u32 x4) and the splatted occluder planes.
constexpr uint32_t kStackChangeCounter = 80;
constexpr uint32_t kStackSplattedPlanes = 96;

// A queued job record: VectorizedCullingJob's four arguments.
struct CullJobRecord {
  be<uint32_t> args[4];
};

// Sk8::VectorizedCullingJob(?, job, ?, ?), frame 15488: params = job+8;
// splats each occluder plane pair (point, normal; 32 bytes, 5 per occluder,
// from params+0) into 6 vectors at sp+96.. (96 bytes per plane), zeroes
// the change counter at sp+80 and calls DoFrustumCulling(job, params,
// sp+96, count, sp+80).
void BodyJob(PPCContext& ctx, uint8_t* base) {
  const uint32_t job = ctx.r4.u32;
  GuestFrame frame(ctx, base, 15488, /*save_r31=*/false);
  const uint32_t sp = ctx.r1.u32;
  const uint32_t params = At<CullJob>(base, job).params;
  const uint32_t count = At<CullingParams>(base, params).occluder_count;
  // Splat every occluder plane (point, normal) into the stack area at sp+96.
  uint32_t src = params + offsetof(CullingParams, occluder_planes), dst = sp + kStackSplattedPlanes;
  for (uint32_t o = 0; o < count; ++o) {
    for (uint32_t p = 0; p < kPlanesPerOccluder;
         ++p, src += sizeof(OccluderPlane), dst += kSplattedPlaneSize) {
      uint8_t img[16];
      const V a = Load(base, src + offsetof(OccluderPlane, point));
      const V n = Load(base, src + offsetof(OccluderPlane, normal));
      const V parts[6] = {Splat<0>(a), Splat<1>(a), Splat<2>(a),
                          Splat<0>(n), Splat<1>(n), Splat<2>(n)};
      for (int k = 0; k < 6; ++k) {
        Image(parts[k], img);
        std::memcpy(Raw(base, dst + 16 * k), img, 16);
      }
    }
  }
  std::memset(Raw(base, sp + kStackChangeCounter), 0, 16);  // the change counter
  // DoFrustumCulling(job, params, splatted planes, occluder count, counter).
  ctx.r3.u64 = job;
  ctx.r4.u64 = params;
  ctx.r5.u64 = sp + kStackSplattedPlanes;
  ctx.r6.u64 = count;
  ctx.r7.u64 = sp + kStackChangeCounter;
  ctx.lr = 0x828171F8;
  sub_82816A40(ctx, base);
}

// BufferedIterativeJobEntryPoint<VectorizedCullingJob>(records, n), frame
// 112: runs the job for each 16-byte record (4 words: r3..r6).
void BodyEntry(PPCContext& ctx, uint8_t* base) {
  const uint32_t records = ctx.r3.u32;
  const int32_t n = ctx.r4.s32;
  GuestFrame frame(ctx, base, 112, /*save_r31=*/false);
  for (int32_t i = 0; i < n; ++i) {
    const auto& rec = At<CullJobRecord>(base, records + sizeof(CullJobRecord) * i);
    ctx.r3.u64 = rec.args[0];
    ctx.r4.u64 = rec.args[1];
    ctx.r5.u64 = rec.args[2];
    ctx.r6.u64 = rec.args[3];
    ctx.lr = 0x82812A68;
    sub_82817138(ctx, base);
  }
}

nr::Family g_cull_job{"cull_job", &BodyMode, nullptr, &BodyJob};
nr::Family g_cull_entry{"cull_ent", &BodyMode, nullptr, &BodyEntry};

}  // namespace

void OnFrameBuilt() { g_mode.store(ReadMode(), std::memory_order_relaxed); }

}  // namespace skate3::native_cull

// DoFrustumCulling (Skate 2 name, sub_82816A40).
extern "C" REX_FUNC(sub_82816A40) {
  skate3::native_replace::Run(skate3::native_cull::g_cull, ctx, base, &__imp__sub_82816A40);
}

// Sk8::VectorizedCullingJob (sub_82817138).
extern "C" REX_FUNC(sub_82817138) {
  skate3::native_replace::Run(skate3::native_cull::g_cull_job, ctx, base, &__imp__sub_82817138);
}

// BufferedIterativeJobEntryPoint<&Sk8::VectorizedCullingJob> (sub_82812A20).
extern "C" REX_FUNC(sub_82812A20) {
  skate3::native_replace::Run(skate3::native_cull::g_cull_entry, ctx, base,
                              &__imp__sub_82812A20);
}
