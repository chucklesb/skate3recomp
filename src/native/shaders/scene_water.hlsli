// Water families, hand-ported from the game's own pixel shaders and
// verified per-pixel against the ucode: flowingwateralpha (fam 30),
// the ocean surface (fam 31) and the baked ocean horizon sheet
// (fam 32). Callers pass the shared world-family locals (squared
// diffuse, fog terms, scene exposure).
// flowingwateralpha (fam 30): the canal/waterfall water shader,
// hand-ported from flowingwateralpha_defaultPS and verified per-pixel
// against the ucode (dual time-scrolled ripple-normal taps -> TBN ->
// lightmap-lit body + phong sun spec + cube reflection, then the
// standard fog/exposure/tonemap chain; alpha = the spec term's green
// channel, floored; reflection strength drives opacity over the bed).
// The program is shared with wateralpha_defaultPS (the water.alpha canal
// materials; byte-for-byte the same ucode except that it outputs alpha 1):
// p0..p3 = the family's own captured m_params rows (wat_* or wa_*),
// opaque = wateralpha's constant alpha.
float4 ShadeWaterProgram(VSOut i, float3 dlin, float fog_a, float3 fog_rgb,
                         float expo, float4 p0, float4 p1, float4 p2,
                         float4 p3, bool opaque) {
  float t = p3.x;
  float3 n1 = macro.Sample(smp, i.uv * p2.xy + p1.xy * t).rgb;
  float3 n2 = macro.Sample(smp, i.uv * p2.zw + p1.zw * t).rgb;
  float3 nsum = 2.0 * n1 + 2.0 * n2 - 2.0;
  // vNormal = normalize(m_params[0].xzw * (n1 + n2)) in tangent space.
  float3 vn = normalize(float3(nsum.x * p0.x, nsum.y * p0.z,
                               nsum.z * p0.w));
  float3 wn0 = dot(i.nrm, i.nrm) > 0.01
                   ? normalize(i.nrm)
                   : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  // Stored frame: tanb = the mesh's usage-6 TANGENT here (see the water
  // pack in DecodeMesh), sentinel sign = the unwrap sign.x;
  // B = sign.x * cross(N, T), exactly the game's VS.
  float3 wt, wb;
  if (i.tanb.w > 0.1) {
    wt = normalize(i.tanb.xyz);
    wb = cross(wn0, wt) * (i.tanb.w > 0.6 ? 1.0 : -1.0);
  } else {
    ScreenTangentFrame(wn0, i.rpos, i.uv, wt, wb);
  }
  float3 wn = normalize(vn.x * wt + vn.y * wb + vn.z * wn0);
  float3 vd = -normalize(i.rpos);
  float s = min(SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.0, wn, i.pos.xy),
                SampleStaticSun(i.rpos + cam_pos.xyz, wn, i.pos.xy));
  // Lightmap at the unwrap + 0.01 * vNormal.xz (the ripple refraction
  // wobble), console fetch semantics, min-clamped like every world
  // family. White-fallback pages skip the clamp (see the env fetch).
  float2 wlm_uv = i.uv2 + 0.01 * vn.xz;
  float3 wlm = lightmap.SampleLevel(smp_clamp, wlm_uv, 0.0).rgb;
  float3 lml = tint.r > 0.0 ? min(wlm * wlm, s + sh_color.rgb)
                            : wlm * wlm;
  // GetTangentLight on the WORLD normal: fixed axis (0.58, 0.62, 0.39)
  // with xy signs following the sun direction, x 1/normalize(axis).z.
  float kd = (wn.x * 0.58 * sign(sh_sun.x) +
              wn.y * 0.62 * sign(sh_sun.y) + wn.z * 0.39) * 2.39562;
  float3 lin = lml * kd * dlin;
  // Spec/reflection masks (t4): x = phong mask, z = reflection mask,
  // thresholded by m_params[3].y. Zero until resolved: matte water.
  float2 wmask = float2(0.0, 0.0);
  if (overlay.x > 0.0) {
    float4 wm4 = decal_art.Sample(smp, i.uv);
    wmask = saturate(float2(wm4.x, wm4.z) - p3.y);
  }
  // Phong vs the real sun, power m_params[3].z, tint (2.1, 1.8, 1.5),
  // scaled by sat(clamped lightmap green - 0.1).
  float3 wrl = 2.0 * wn * dot(wn, sh_sun.xyz) - sh_sun.xyz;
  float ks = pow(max(saturate(dot(vd, wrl)), 1e-6), p3.z);
  float3 wspec =
      ks * wmask.x * float3(2.1, 1.8, 1.5) * saturate(lml.g - 0.1);
  // Cube reflection, same shape as fams 5/6: reflect(E, wN) with xy
  // negated, luminosity lerped toward 1 by 0.3 * sat(4*mask - 2.6),
  // x mask x 1.5. misc.y = the cube LOD bias (640p-gradient parity).
  float3 wrv = vd - 2.0 * wn * dot(vd, wn);
  float3 wcube =
      overlay.y > 0.0
          ? env_cube.SampleBias(smp, float3(-wrv.x, -wrv.y, wrv.z),
                                misc.y).rgb
          : float3(0.0, 0.0, 0.0);
  float wrlum = 0.3 * saturate(4.0 * wmask.y - 2.6);
  float3 wrefl = wcube * (lml.g + wrlum * (1.0 - lml.g)) * wmask.y * 1.5;
  float3 wspec_term = wspec + wrefl;
  lin += wspec_term;
  float3 wxe = (lin * (fog_a * p0.y) + fog_rgb) * expo;
  return ToneOut(wxe, opaque ? 1.0 : max(wspec_term.g, p3.w), false);
}

float4 ShadeFlowingWater(VSOut i, float3 dlin, float fog_a,
                         float3 fog_rgb, float expo) {
  return ShadeWaterProgram(i, dlin, fog_a, fog_rgb, expo, wat_p0, wat_p1,
                           wat_p2, wat_p3, false);
}

// ---- wateralpha (fam 33): the water.alpha canal materials ----------------
// Hand-ported from wateralpha_defaultPS (Skate 3 ucode, dumped live; the
// literal constants c249..c255 read from the shader's own constant block:
// (-0.99,-2,-2.6,0.99) (0.75,-1,2,2.39562) (0.5,2/3,1/6,0.02) (1,0,2.1,1.8)
// (1/6,-0.5,0.25,0.3) (0.01,4,0.39,0.5) (0.58,0.62,1.5,0.1)).
// The normal is a PCA reconstruction from TWO component maps (`normal` at
// t3, `normal2` at t8), each fetched at two scales:
//   tap A: uv * c20.zw + c19.zw * t (the shading normal; c19.zw = 0, so
//          static texels whose per-frame PCA weights c9..c14 animate the
//          waves),
//   tap B: uv * c20.xy + c19.xy * t (slow scroll; only displaces the
//          diffuse / mask lookups, the refraction wobble).
// component = 2 * (c8 + dot(2a-1, wA) + dot(2b-1, wB)) + (-1, -2, -1),
// world N = normalize(s * component + (0, 1, 0)), s = c18.z (A) / c18.w (B).
// Then the same lighting as flowingwateralpha: lightmap (4-tap box at
// uv2 + 0.01 * N.xz, squared, clamped by shadow + shadow color c15), the
// fixed-axis tangent light, phong sun spec (power c21.z), cube reflection,
// fog x c18.y, exposure. Output alpha 1 (the canal hides its bed).
float3 WaterAlphaPca(float2 uv) {
  float4 a = macro.Sample(smp, uv) * 2.0 - 1.0;
  float4 b = detail_map.Sample(smp, uv) * 2.0 - 1.0;
  float3 p = wa_c8.xyz + float3(dot(a, wa_c9) + dot(b, wa_c10),
                                dot(a, wa_c11) + dot(b, wa_c12),
                                dot(a, wa_c13) + dot(b, wa_c14));
  return 2.0 * p + float3(-1.0, -2.0, -1.0);
}

// De-tiling (wa_ctl.z > 0, not in the game): the static tap A repeats every
// UV unit across the canal. Blend it with a second copy of the same field
// rotated and offset, weighted by smooth low-frequency world-space noise, so
// no two neighbouring repeats look alike. Every copy is a valid PCA normal
// field, so the blend stays a plausible surface.
float WaterHash(float2 p) {
  return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}
float WaterNoise(float2 p) {
  float2 c = floor(p);
  float2 f = frac(p);
  f = f * f * (3.0 - 2.0 * f);
  return lerp(lerp(WaterHash(c), WaterHash(c + float2(1, 0)), f.x),
              lerp(WaterHash(c + float2(0, 1)), WaterHash(c + float2(1, 1)), f.x),
              f.y);
}
float3 WaterAlphaTapA(float2 uvA, float3 wp) {
  float3 n = WaterAlphaPca(uvA);
  if (wa_ctl.z > 0.0) {
    // Second copy: rotated ~37 degrees and shifted; PCA x/z components
    // rotate with the UVs (the field is authored in uv-aligned axes).
    const float2x2 rot = float2x2(0.799, -0.602, 0.602, 0.799);
    float3 n2 = WaterAlphaPca(mul(rot, uvA) + float2(0.37, 0.71));
    n2.xz = mul(n2.xz, rot);
    float w = smoothstep(0.25, 0.75, WaterNoise(wp.xz * 0.11)) * saturate(wa_ctl.z);
    // Two uncorrelated fields averaged lose amplitude (up to 1/sqrt(2) at
    // w = 0.5, a visibly softer surface); renormalise the deviation.
    n = lerp(n, n2, w) * rsqrt((1.0 - w) * (1.0 - w) + w * w);
  }
  return n;
}

float4 ShadeWaterAlpha(VSOut i, float fog_a, float3 fog_rgb, float expo) {
  float t = wa_c22.x;
  float2 uvs = i.uv * wa_c21.x;
  float2 uvA = uvs * wa_c20.zw + wa_c19.zw * t;
  float2 uvB = uvs * wa_c20.xy + wa_c19.xy * t;
  float3 wp = i.rpos + cam_pos.xyz;
  float3 cA = WaterAlphaTapA(uvA, wp);
  float3 cB = WaterAlphaPca(uvB);
  float3 N = normalize(cA * wa_c18.z + float3(0.0, 1.0, 0.0));
  float3 nB = normalize(cB * wa_c18.w + float3(0.0, 1.0, 0.0));
  // Refraction wobble of the diffuse and mask lookups (c251.w = 0.02).
  float2 uvR = i.uv + 0.02 * nB.xz;
  float3 dl = diffuse.Sample(smp, uvR).rgb;
  dl *= dl;
  float2 wmask = float2(0.0, 0.0);
  if (overlay.x > 0.0) {
    float4 m = decal_art.Sample(smp, uvR);
    wmask = saturate(float2(m.x, m.z) - wa_c21.y);
  }
  float3 E = -normalize(i.rpos);
  float s = min(SampleCsmShadowSoft(wp, 0.0, N, i.pos.xy),
                SampleStaticSun(wp, N, i.pos.xy));
  // Lightmap: 4-tap +-0.5 texel box at uv2 + 0.01 * N.xz, then squared.
  float2 lmuv = i.uv2 + 0.01 * N.xz;
  float lw, lh;
  lightmap.GetDimensions(lw, lh);
  float2 ht = 0.5 / max(float2(lw, lh), 1.0);
  float3 lm = 0.25 * (lightmap.SampleLevel(smp_clamp, lmuv + float2(ht.x, ht.y), 0.0).rgb +
                      lightmap.SampleLevel(smp_clamp, lmuv + float2(-ht.x, ht.y), 0.0).rgb +
                      lightmap.SampleLevel(smp_clamp, lmuv + float2(-ht.x, -ht.y), 0.0).rgb +
                      lightmap.SampleLevel(smp_clamp, lmuv + float2(ht.x, -ht.y), 0.0).rgb);
  lm *= lm;
  float3 lml = tint.r > 0.0 ? min(lm, s + wa_c15.rgb) : lm;
  float kd = (N.x * 0.58 * sign(wa_sun.x) + N.y * 0.62 * sign(wa_sun.y) +
              N.z * 0.39) * 2.39562;
  float3 lin = lml * kd * dl;
  // Phong sun spec (power c21.z), tint (2.1, 1.8, 1.5), x sat(lml.g - 0.1).
  float3 Ls = wa_sun.xyz;
  float ks = pow(max(saturate(dot(E, 2.0 * N * dot(N, Ls) - Ls)), 1e-6), wa_c21.z);
  float3 spec = ks * wmask.x * float3(2.1, 1.8, 1.5) * saturate(lml.g - 0.1);
  // Cube reflection: reflect E about N, xy negated (the game's cube space).
  float3 R = E - 2.0 * N * dot(E, N);
  float3 cube = overlay.y > 0.0
                    ? env_cube.SampleBias(smp, float3(-R.x, -R.y, R.z), misc.y).rgb
                    : float3(0.0, 0.0, 0.0);
  float rl = 0.3 * saturate(4.0 * wmask.y - 2.6);
  float3 refl = cube * (lml.g + rl * (1.0 - lml.g)) * wmask.y * 1.5;
  lin += spec + refl;
  // Debug views (skate3_native_render_scene_water_debug, wa_ctl.w).
  int dbg = int(wa_ctl.w + 0.5);
  if (dbg > 0) {
    float3 v = dbg == 1   ? dl
               : dbg == 2 ? float3(wmask, 0.0)
               : dbg == 3 ? lml
               : dbg == 4 ? cube
               : dbg == 5 ? N * 0.5 + 0.5
               : dbg == 6 ? float3(saturate(kd * 0.25), s, saturate(lm.g))
                          : float3(fog_a * wa_c18.y, expo * 0.25, 0.0);
    return float4(v, 1.0);
  }
  // Opaque: the canals have no bed geometry under the surface (a black
  // void; the game's murk hides it), so see-through water has nothing to
  // show.
  float3 xe = (lin * (fog_a * wa_c18.y) + fog_rgb) * expo;
  return ToneOut(xe, 1.0, false);
}

// The canal shading normal (SSR G-buffer restage).
float3 WaterAlphaNormal(VSOut i) {
  float2 uvA = i.uv * wa_c21.x * wa_c20.zw + wa_c19.zw * wa_c22.x;
  return normalize(WaterAlphaTapA(uvA, i.rpos + cam_pos.xyz) * wa_c18.z +
                   float3(0.0, 1.0, 0.0));
}

// Exact ocean surface (fam 31, ocean_defaultPS; verified per-pixel
// against the ucode): PCA-compressed dual-component normal map, lerped
// toward flat by the overlay-scaled tonedown, cube reflection x squared
// lightmap x fresnel, ward anisotropic sun spec. OPAQUE (the game's PS
// outputs alpha 0 and draws without blending).
float4 ShadeOcean(VSOut i, float fog_a, float3 fog_rgb, float expo) {
  float2 suv = i.uv * oc_p1.w;
  float4 c0v = macro.Sample(smp, suv) * 2.0 - 1.0;       // t3: comp 0
  float4 c1v = detail_map.Sample(smp, suv) * 2.0 - 1.0;  // t8: comp 1
  // PCA normal in the shader's Z-UP basis; mean/weights are stored in
  // 0..1 texture space and folded 2x-1 after the sum.
  float3 pca = (float3(dot(c0v, oc_w0) + dot(c1v, oc_w1),
                       dot(c0v, oc_w2) + dot(c1v, oc_w3),
                       dot(c0v, oc_w4) + dot(c1v, oc_w5)) +
                oc_mean.xyz) * 2.0 - 1.0;
  // tonedown = 2 * m_params[1].z * sat(dot(c1v, wB1) + overlay.x);
  // the ucode folds the second PCA blue dot into the overlay term.
  float ovx = overlay.z > 0.0
                  ? spec2_map.Sample(smp, i.uv * overlay.w).x
                  : 1.0;
  float otd = (2.0 * oc_p1.z) * saturate(dot(c1v, oc_w5) + ovx);
  float3 nt = normalize(lerp(float3(0.0, 0.0, 1.0), pca, otd));
  nt = normalize(max(abs(nt), 0.001));
  float3 N = float3(nt.x, nt.z, nt.y);  // shader Z-up -> world Y-up
  float3 E = -normalize(i.rpos);
  // Cube reflection: reflect(E, N) with xy negated, x the SQUARED
  // single-tap lightmap (no chromaticity, no CSM clamp in this PS).
  float3 orv = E - 2.0 * N * dot(E, N);
  float3 ocube =
      overlay.y > 0.0
          ? env_cube.SampleBias(smp, float3(-orv.x, -orv.y, orv.z),
                                misc.y).rgb
          : float3(0.0, 0.0, 0.0);
  float2 olm_uv = i.uv2 + 0.01 * nt.xy;
  float3 olm = lightmap.SampleLevel(smp_clamp, olm_uv, 0.0).rgb;
  float3 orefl = ocube * (olm * olm);
  float NV = abs(dot(N, E));
  float F = 0.25 + 0.75 * pow(1.0 - NV, 5.0);
  // Ward anisotropic sun spec (spreads oc_p2.xy, roughness oc_p2.z).
  float3 oud = normalize(float3(-N.z, 0.0, N.x));
  float3 ovd = cross(N, oud);
  float dNE = dot(N, E);
  float dNL = dot(N, sh_sun.xyz);
  float oax = oc_p2.z / oc_p2.x;
  float oay = oc_p2.z / oc_p2.y;
  float3 H = normalize(sh_sun.xyz + E);
  float eu = dot(H, oud) / oax;
  float ev = dot(H, ovd) / oay;
  float oden = sqrt(abs(dNL * dNE)) * 12.566371 * oax * oay;
  float ward = exp(-2.0 * (eu * eu + ev * ev) / (1.0 + dot(H, N))) /
               max(oden, 1e-6);
  float3 ocol = orefl * F + ward * oc_p0.rgb;
  float3 oxe = (ocol * (fog_a * oc_p1.y) + fog_rgb) * expo;
  return ToneOut(oxe, 1.0, false);

}

// Baked ocean horizon sheet (fam 32, oceanreflection_defaultPS;
// verified per-pixel): squared sea texture x multiplier, fogged and
// tonemapped; alpha ramps with world height between the orf.z/orf.y
// bounds (the bright haze band fading in toward the horizon).
float4 ShadeOceanReflection(VSOut i, float3 dlin, float fog_a,
                            float3 fog_rgb, float expo) {
  float3 rcol = dlin * orf.x;
  float ra = saturate((i.rpos.y + cam_pos.y - orf.z) /
                      max(orf.y - orf.z, 1e-4));
  float3 rxe = (rcol * fog_a + fog_rgb) * expo;
  return ToneOut(rxe, ra, false);

}
