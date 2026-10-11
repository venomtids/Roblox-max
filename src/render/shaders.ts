/**
 * ============================================================================
 *  SHADERS WGSL — o coração visual da plataforma
 * ============================================================================
 *  Três pipelines, todos com o mesmo princípio: o VÉRTICE É UM u32 PACKED.
 *  Em vez de 5-6 atributos float (24-32 B/vértice), transferimos 16 B e
 *  desempacotamos na GPU (bit ops de 1 ciclo). Resultado: 30-50% menos
 *  banda de memória de vértice — o gargalo real de mundos voxel.
 *
 *  Layout do vértice (16 B, 4 × u32):
 *   wordA : x (u16) | y (u16)
 *   wordB : z (u16) | normal (u3 << 16) | AO (u2 << 19) | emissivo (u4 << 21)
 *   wordC : u (u8) | v (u8) | camada do atlas (u16)
 *   wordD : skyLight (u8) | blockLight (u8) | tint (u8) | reservado
 */

export const CHUNK_WGSL = /* wgsl */ `
struct Cam {
  viewProj   : mat4x4<f32>,
  camPosTime : vec4<f32>,   // xyz = posição da câmera, w = tempo (s)
  fogColor   : vec4<f32>,   // rgb = cor do horizonte, a = densidade do fog
  sunDir     : vec4<f32>,   // xyz = direção do sol, w = luz ambiente
  params     : vec4<f32>,   // x = nível do mar, y = amplitude da onda, z/w livres
};

@group(0) @binding(0) var<uniform> cam : Cam;
@group(0) @binding(1) var atlas : texture_2d_array<f32>;
@group(0) @binding(2) var atlasSampler : sampler;

struct VSOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) uv      : vec2<f32>,
  @location(1) layer   : f32,
  @location(2) normal  : f32,
  @location(3) ao      : f32,
  @location(4) light   : f32,
  @location(5) tint    : f32,
  @location(6) world   : vec3<f32>,
};

@vertex
fn vsChunk(
  @location(0) wa : u32,
  @location(1) wb : u32,
  @location(2) wc : u32,
  @location(3) wd : u32,
) -> VSOut {
  var o : VSOut;

  // ── Desempacotamento (3 op por componente, tudo em registradores) ──────
  var p = vec3<f32>(
    f32(wa & 0xffffu),
    f32((wa >> 16u) & 0xffffu),
    f32(wb & 0xffffu),
  );
  let layer  = wc >> 16u;
  let normal = f32((wb >> 16u) & 7u);

  // ── Onda procedural na água (camada 5) — custo ~0, roda na GPU ─────────
  if (layer == 5u) {
    let t = cam.camPosTime.w;
    let amp = cam.params.y;
    p.y += (sin(p.x * 0.45 + t * 1.7) + sin(p.z * 0.37 - t * 1.25)) * amp;
  }

  o.world  = p;
  o.pos    = cam.viewProj * vec4<f32>(p, 1.0);
  o.uv     = vec2<f32>(f32(wc & 0xffu), f32((wc >> 8u) & 0xffu)) * (1.0 / 32.0);
  o.layer  = f32(layer);
  o.normal = normal;
  o.ao     = f32((wb >> 19u) & 3u);
  o.light  = f32(wd & 0xffu);
  o.tint   = f32((wd >> 8u) & 0xffu) / 15.0;
  return o;
}

// Brilho por face: tabela em registradores (sem lookup de memória).
fn faceShade(n : f32) -> f32 {
  if (n < 0.5) { return 1.00; }        // +X
  if (n < 1.5) { return 0.78; }        // -X
  if (n < 2.5) { return 1.12; }        // +Y (topo)
  if (n < 3.5) { return 0.58; }        // -Y (base)
  if (n < 4.5) { return 0.90; }        // +Z
  return 0.70;                          // -Z
}

@fragment
fn fsChunk(i : VSOut) -> @location(0) vec4<f32> {
  let tex = textureSampleLevel(atlas, atlasSampler, i.uv, i32(i.layer), 0.0);

  // Alpha test duro: folhas/vidro não usam blend, então descartamos cedo.
  if (tex.a < 0.35) { discard; }

  // ── Iluminação: face + AO + luz emitida ────────────────────────────────
  let ao    = 0.55 + 0.15 * i.ao;                    // 0..3 → 0.55..1.0
  let sky   = 0.42 + 0.58 * (i.light / 15.0);        // céu/luz de bloco
  let sh    = faceShade(i.normal) * ao * sky;
  var rgb   = tex.rgb * sh;

  // Emissivo (lâmpada) — quebra a oclusão e "brilha"
  if (i.tint > 0.0) {
    rgb = mix(rgb, tex.rgb * 1.35, i.tint);
  }

  // ── Fog exponencial ao quadrado (profundidade) + haze do horizonte ─────
  let toCam  = cam.camPosTime.xyz - i.world;
  let dist   = length(toCam);
  let density = cam.fogColor.a;
  let fog    = 1.0 - exp(-density * density * dist * dist);
  rgb = mix(rgb, cam.fogColor.rgb, clamp(fog, 0.0, 1.0));

  return vec4<f32>(rgb, tex.a);
}
`;

export const SKY_WGSL = /* wgsl */ `
struct Sky {
  forward : vec4<f32>,   // xyz = direção da câmera, w = tan(fov/2)
  right   : vec4<f32>,   // xyz = direita da câmera, w = aspect
  up      : vec4<f32>,
  sunDir  : vec4<f32>,   // xyz = sol, w = intensidade
  top     : vec4<f32>,   // rgb do zênite
  horizon : vec4<f32>,   // rgb do horizonte
  bottom  : vec4<f32>,   // rgb abaixo do horizonte
  res     : vec4<f32>,   // xy = resolução
};
@group(0) @binding(0) var<uniform> sky : Sky;

struct VSOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) ndc : vec2<f32>,
};

@vertex
fn vsSky(@builtin(vertex_index) vi : u32) -> VSOut {
  // Triângulo gigante: 3 vértices cobrem a tela toda, sem vertex buffer.
  var xy = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -3.0),
    vec2<f32>(-1.0,  1.0),
    vec2<f32>( 3.0,  1.0),
  );
  var o : VSOut;
  o.pos = vec4<f32>(xy[vi], 1.0, 1.0);
  o.ndc = xy[vi];
  return o;
}

// Hash barato para as estrelas (só no lado noturno é usado).
fn hash21(p : vec2<f32>) -> f32 {
  var h = dot(p, vec2<f32>(127.1, 311.7));
  return fract(sin(h) * 43758.5453);
}

@fragment
fn fsSky(i : VSOut) -> @location(0) vec4<f32> {
  let aspect = sky.right.w;
  let tanHalf = sky.forward.w;
  let dir = normalize(
      sky.forward.xyz
    + sky.right.xyz * (i.ndc.x * aspect * tanHalf)
    + sky.up.xyz    * (i.ndc.y * tanHalf)
  );

  // Gradiente por elevação + disco solar
  let up01 = clamp(dir.y, -1.0, 1.0);
  var col = select(
    mix(sky.horizon.rgb, sky.bottom.rgb, clamp(-up01 * 2.0, 0.0, 1.0)),
    mix(sky.horizon.rgb, sky.top.rgb, pow(up01, 0.62)),
    up01 > 0.0
  );

  let sunAmt = max(dot(dir, normalize(sky.sunDir.xyz)), 0.0);
  col += vec3<f32>(1.0, 0.94, 0.80) * pow(sunAmt, 220.0) * 2.4 * max(sky.sunDir.w, 0.0);
  col += vec3<f32>(1.0, 0.82, 0.55) * pow(sunAmt, 8.0) * 0.16 * max(sky.sunDir.w, 0.0);

  // Estrelas (aparecem quando o sol está baixo; custo desprezível)
  if (sky.sunDir.w < 0.35 && dir.y > 0.02) {
    let cell = floor(dir.xz * 260.0);
    let s = hash21(cell);
    if (s > 0.9985) { col += vec3<f32>(0.9, 0.95, 1.0) * 1.4; }
  }

  return vec4<f32>(col, 1.0);
}
`;

export const BODIES_WGSL = /* wgsl */ `
struct Cam {
  viewProj   : mat4x4<f32>,
  camPosTime : vec4<f32>,
  fogColor   : vec4<f32>,
  sunDir     : vec4<f32>,
  params     : vec4<f32>,
};
@group(0) @binding(0) var<uniform> cam : Cam;

struct VSOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) nrm : vec3<f32>,
  @location(1) col : vec4<f32>,
  @location(2) world : vec3<f32>,
};

@vertex
fn vsBody(
  @location(0) pos : vec3<f32>,
  @location(1) nrm : vec3<f32>,
  @location(2) ipos : vec3<f32>,
  @location(3) iscale : f32,
  @location(4) icolor : vec4<f32>,
) -> VSOut {
  var o : VSOut;
  let p = ipos + pos * iscale;
  o.pos = cam.viewProj * vec4<f32>(p, 1.0);
  o.nrm = nrm;
  o.col = icolor;
  o.world = p;
  return o;
}

@fragment
fn fsBody(i : VSOut) -> @location(0) vec4<f32> {
  let n = normalize(i.nrm);
  let sun = normalize(cam.sunDir.xyz);
  let lam = max(dot(n, sun), 0.0) * 0.65 + cam.sunDir.w * 0.45;
  var rgb = i.col.rgb * (0.42 + lam);
  let dist = length(cam.camPosTime.xyz - i.world);
  let fog = 1.0 - exp(-cam.fogColor.a * cam.fogColor.a * dist * dist);
  rgb = mix(rgb, cam.fogColor.rgb, clamp(fog, 0.0, 1.0));
  return vec4<f32>(rgb, 1.0);
}
`;

export const OUTLINE_WGSL = /* wgsl */ `
struct Cam {
  viewProj   : mat4x4<f32>,
  camPosTime : vec4<f32>,
  fogColor   : vec4<f32>,
  sunDir     : vec4<f32>,
  params     : vec4<f32>,
};
@group(0) @binding(0) var<uniform> cam : Cam;

@vertex
fn vsLine(@location(0) p : vec3<f32>) -> @builtin(position) vec4<f32> {
  return cam.viewProj * vec4<f32>(p, 1.0);
}

@fragment
fn fsLine() -> @location(0) vec4<f32> {
  return vec4<f32>(0.20, 1.0, 0.55, 0.95);
}
`;
