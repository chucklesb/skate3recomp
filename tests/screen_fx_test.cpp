#include "native/screen_fx.h"
#include "test_check.h"

#include <limits>

using namespace skate3::native_scene;

ScreenFxState ValidCapture(ScreenFxVariant variant) {
  ScreenFxState state;
  state.variant = variant;
  state.scaled_uv = true;
  state.vs[3][0] = state.vs[3][1] = 1.0f;
  for (unsigned slot : {0u, 2u}) {
    state.fetch[slot][0] = 2;
    state.fetch[slot][1] = 0x100054;
  }
  state.ps[1][1] = 1.0f;
  return state;
}

void TestShaderIdentity() {
  CHECK(ScreenFxShaderObjectHeadersValid(6, 0x102a1101, 7, 0x102a1100));
  CHECK(ScreenFxShaderObjectHeadersValid(6, 0x102a1141, 7, 0x102a1100));
  CHECK(!ScreenFxShaderObjectHeadersValid(7, 0x102a1101, 6, 0x102a1100));
  CHECK(!ScreenFxShaderObjectHeadersValid(6, 0x102a1100, 7, 0x102a1100));
  CHECK(!ScreenFxShaderObjectHeadersValid(6, 0x102a1101, 7, 0x102a1101));
  CHECK(!ScreenFxShaderObjectHeadersValid(6, 0x102a1101, 7, 0x102a1140));
  for (unsigned unsupported : {0x2u, 0x4u, 0x8u, 0x10u, 0x20u, 0x80u,
                               0x10000u, 0x1000000u}) {
    CHECK(!ScreenFxShaderObjectHeadersValid(
        6, 0x102a1101 | unsupported, 7, 0x102a1100));
    CHECK(!ScreenFxShaderObjectHeadersValid(
        6, 0x102a1141 | unsupported, 7, 0x102a1100));
  }
  CHECK(kScreenFxVsPathOffset == kScreenFxVsHeaderOffset + 44);
  CHECK(kScreenFxPsPathOffset == kScreenFxPsHeaderOffset + 44);
  CHECK(ClassifyScreenFxPixelShader(
      "D:\\shaders\\postfx_basictex_fisheye_opaquePS.updb") ==
      ScreenFxVariant::kOpaque);
  CHECK(ClassifyScreenFxPixelShader(
      "/shaders/postfx_basictex_fisheye_noisePS.updb") == ScreenFxVariant::kNone);
  CHECK(ClassifyScreenFxPixelShader("postfx_basictex_fisheyePS.updb") ==
      ScreenFxVariant::kAlpha);
  for (const char* path : {
      "postfx_basictex_fisheye_opaquePS.updb/otherPS.updb",
      "prefix_postfx_basictex_fisheyePS.updb",
      "postfx_basictex_fisheye_noisePS.updb.stale",
      "postfx_basictex_fisheyeVS.updb", "postfx_basictex.updb", ""}) {
    CHECK(ClassifyScreenFxPixelShader(path) == ScreenFxVariant::kNone);
  }
  CHECK(ClassifyScreenFxVertexShader("postfx_quadTransformVS.updb") ==
         ScreenFxVertex::kScaled);
  CHECK(ClassifyScreenFxVertexShader("postfx_defaultVS.updb") ==
         ScreenFxVertex::kRaw);
  CHECK(ClassifyScreenFxVertexShader("prefix_postfx_quadTransformVS.updb") ==
         ScreenFxVertex::kNone);
  // Re-reading a recycled object must distinguish its new leaf every time.
  CHECK(ClassifyScreenFxPixelShader("postfx_basictex_fisheyePS.updb") !=
         ClassifyScreenFxPixelShader("postfx_basictex_fisheye_noisePS.updb"));
}

void TestBindingsAndUsedLanes() {
  const float invalid = std::numeric_limits<float>::quiet_NaN();
  for (auto variant : {ScreenFxVariant::kOpaque, ScreenFxVariant::kAlpha}) {
    auto state = ValidCapture(variant);
    CHECK(ScreenFxCaptureValid(state));
    state.ps[0][2] = state.ps[1][3] = state.ps[2][0] = invalid;
    state.ps[2][3] = state.vs[3][3] = invalid;
    CHECK(ScreenFxCaptureValid(state));
    for (unsigned lane : {0u, 1u, 3u, 4u, 5u, 6u, 9u}) {
      auto bad = state;
      bad.ps[lane / 4][lane % 4] = invalid;
      CHECK(!ScreenFxCaptureValid(bad));
    }
    for (unsigned slot : {0u, 2u}) {
      auto bad = state;
      bad.fetch[slot][1] = 0x54;  // format bits alone are not an address
      CHECK(!ScreenFxCaptureValid(bad));
      bad = state;
      bad.fetch[slot][0] = 3;
      CHECK(!ScreenFxCaptureValid(bad));
    }
    auto bad = state;
    bad.vs[2][0] = invalid;
    CHECK(!ScreenFxCaptureValid(bad));
    bad = state;
    bad.vs[3][1] = 0;
    CHECK(!ScreenFxCaptureValid(bad));
    bad.scaled_uv = false;  // raw VS does not consume its staging rows
    bad.vs[2][0] = invalid;
    CHECK(ScreenFxCaptureValid(bad));
  }

}

void TestFrameExpiration() {
  auto pending = ValidCapture(ScreenFxVariant::kAlpha);
  pending.ps[0][0] = 0.2f;
  pending.ps[0][1] = 0.8f;
  pending.render_states[1] = 0x00010001;
  pending.color_mask = 0xf;
  auto first = TakeScreenFxCapture(pending, 41);
  CHECK(first.variant == ScreenFxVariant::kAlpha && first.generation == 41);
  CHECK(first.ps[0][1] == 0.8f && first.fetch[2][1] == 0x100054);
  CHECK(first.render_states[1] == 0x00010001 && first.color_mask == 0xf);
  CHECK(pending.variant == ScreenFxVariant::kNone);
  // The world can be held while the independent effect changes or ends.
  const auto held_world_effect = first;
  auto second = TakeScreenFxCapture(pending, 42);
  CHECK(held_world_effect.variant == ScreenFxVariant::kAlpha);
  CHECK(second.variant == ScreenFxVariant::kNone && second.generation == 42);
  pending = ValidCapture(ScreenFxVariant::kOpaque);
  auto third = TakeScreenFxCapture(pending, 43);
  CHECK(third.variant == ScreenFxVariant::kOpaque && third.generation == 43);
  pending = ValidCapture(ScreenFxVariant::kAlpha);
  pending.fetch[2][1] = 0;
  auto missing_binding = TakeScreenFxCapture(pending, 44);
  CHECK(missing_binding.variant == ScreenFxVariant::kNone);
  CHECK(pending.variant == ScreenFxVariant::kNone);
}

void TestBaseMapSamplerLod() {
  uint32_t fetch[6] = {};
  fetch[3] = 2u << 23;
  CHECK(ScreenFxBaseMapLod(fetch) == 0.0f);
  for (unsigned minimum : {1u, 15u}) {
    fetch[4] = minimum << 2;
    CHECK(ScreenFxBaseMapLod(fetch) == 0.0f);
    fetch[5] = 0xe0000fff;  // unrelated bits are not a physical mip page
    CHECK(ScreenFxBaseMapLod(fetch) == 0.0f);
    fetch[5] = 0;
  }
  fetch[5] = 1u << 12;  // a real mip page enables the declared minimum
  fetch[4] = 1u << 2;
  CHECK(ScreenFxBaseMapLod(fetch) == 1.0f);
  fetch[4] = 15u << 2;
  CHECK(ScreenFxBaseMapLod(fetch) == 15.0f);
  // Changing unrelated texture/sampler fields must not alter the level.
  fetch[0] = fetch[1] = fetch[2] = fetch[5] = 0xa5a5a5a5;
  fetch[3] |= ~(3u << 23);
  fetch[4] |= ~(15u << 2);
  const uint32_t before[6] = {fetch[0], fetch[1], fetch[2],
                             fetch[3], fetch[4], fetch[5]};
  CHECK(ScreenFxBaseMapLod(fetch) == 15.0f);
  for (unsigned i = 0; i < 6; ++i) CHECK(fetch[i] == before[i]);
  for (unsigned filter : {0u, 1u, 3u}) {
    fetch[3] = (fetch[3] & ~(3u << 23)) | (filter << 23);
    CHECK(ScreenFxBaseMapLod(fetch) == -1.0f);
    fetch[4] &= ~(15u << 2);
    CHECK(ScreenFxBaseMapLod(fetch) == -1.0f);
  }
}

int main() {
  TestShaderIdentity();
  TestBindingsAndUsedLanes();
  TestFrameExpiration();
  TestBaseMapSamplerLod();
  return skate3_test::TestExit(
      "PASS: exact screen shader identities, used lanes/bindings, "
      "independent frame expiration, and BaseMap sampler LOD");
}
