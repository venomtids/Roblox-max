/**
 * ============================================================================
 *  RUÍDO PROCEDURAL (hash inteiro + value noise + fBm)
 * ============================================================================
 *  Sem tabelas em memória (zero data segment, zero cache miss): um hash
 *  inteiro de 3 multiplicações é mais rápido que ler uma permutation table
 *  e é 100% determinístico entre workers/instâncias WASM.
 */

/** Hash inteiro 2D → inteiro em [0, 2^32). */
export function hash2(x: i32, y: i32, seed: i32): u32 {
  let h: u32 = (u32(x) * 0x27d4eb2d) ^ (u32(y) * 0x165667b1) ^ (u32(seed) * 0x9e3779b1);
  h ^= h >>> 15;
  h *= 0x2c1b3c6d;
  h ^= h >>> 12;
  h *= 0x297a2d39;
  h ^= h >>> 15;
  return h;
}

/** Hash inteiro 3D → inteiro em [0, 2^32). */
export function hash3(x: i32, y: i32, z: i32, seed: i32): u32 {
  let h: u32 = (u32(x) * 0x27d4eb2d) ^ (u32(y) * 0x165667b1) ^ (u32(z) * 0x1b873593) ^ (u32(seed) * 0x9e3779b1);
  h ^= h >>> 13;
  h *= 0x85ebca6b;
  h ^= h >>> 16;
  return h;
}

/** Hash 2D → float em [0,1). */
@inline function rand2(x: i32, y: i32, seed: i32): f32 {
  return f32(hash2(x, y, seed) >>> 8) * (1.0 / 16777216.0); // 24 bits de mantissa
}

/** Hash 3D → float em [0,1). */
@inline function rand3(x: i32, y: i32, z: i32, seed: i32): f32 {
  return f32(hash3(x, y, z, seed) >>> 8) * (1.0 / 16777216.0);
}

/** Curva de suavização (quintic) — C2 contínua, evita "terracing" no terreno. */
@inline function smooth(t: f32): f32 {
  return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

@inline function lerp(a: f32, b: f32, t: f32): f32 {
  return a + (b - a) * t;
}

/** Value noise 2D em [-1,1]. */
export function vnoise2(x: f32, y: f32, seed: i32): f32 {
  const xi: i32 = i32(Mathf.floor(x));
  const yi: i32 = i32(Mathf.floor(y));
  const fx: f32 = smooth(x - f32(xi));
  const fy: f32 = smooth(y - f32(yi));
  const v00: f32 = rand2(xi, yi, seed);
  const v10: f32 = rand2(xi + 1, yi, seed);
  const v01: f32 = rand2(xi, yi + 1, seed);
  const v11: f32 = rand2(xi + 1, yi + 1, seed);
  return lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fy) * 2.0 - 1.0;
}

/** Value noise 3D em [-1,1] (usado para cavernas). */
export function vnoise3(x: f32, y: f32, z: f32, seed: i32): f32 {
  const xi: i32 = i32(Mathf.floor(x));
  const yi: i32 = i32(Mathf.floor(y));
  const zi: i32 = i32(Mathf.floor(z));
  const fx: f32 = smooth(x - f32(xi));
  const fy: f32 = smooth(y - f32(yi));
  const fz: f32 = smooth(z - f32(zi));
  const c000: f32 = rand3(xi, yi, zi, seed);
  const c100: f32 = rand3(xi + 1, yi, zi, seed);
  const c010: f32 = rand3(xi, yi + 1, zi, seed);
  const c110: f32 = rand3(xi + 1, yi + 1, zi, seed);
  const c001: f32 = rand3(xi, yi, zi + 1, seed);
  const c101: f32 = rand3(xi + 1, yi, zi + 1, seed);
  const c011: f32 = rand3(xi, yi + 1, zi + 1, seed);
  const c111: f32 = rand3(xi + 1, yi + 1, zi + 1, seed);
  const x00: f32 = lerp(c000, c100, fx);
  const x10: f32 = lerp(c010, c110, fx);
  const x01: f32 = lerp(c001, c101, fx);
  const x11: f32 = lerp(c011, c111, fx);
  return lerp(lerp(x00, x10, fy), lerp(x01, x11, fy), fz) * 2.0 - 1.0;
}

/** fBm 2D (soma de oitavas) — o "esqueleto" do terreno. */
export function fbm2(x: f32, y: f32, octaves: i32, seed: i32): f32 {
  let sum: f32 = 0.0;
  let amp: f32 = 1.0;
  let freq: f32 = 1.0;
  let norm: f32 = 0.0;
  for (let i: i32 = 0; i < octaves; i++) {
    sum += vnoise2(x * freq, y * freq, seed + i * 1013) * amp;
    norm += amp;
    amp *= 0.5;
    freq *= 2.03; // 2.03 (e não 2.0) quebra o alinhamento de grade entre oitavas
  }
  return sum / norm;
}

/** fBm 3D (cavernas / veios). */
export function fbm3(x: f32, y: f32, z: f32, octaves: i32, seed: i32): f32 {
  let sum: f32 = 0.0;
  let amp: f32 = 1.0;
  let freq: f32 = 1.0;
  let norm: f32 = 0.0;
  for (let i: i32 = 0; i < octaves; i++) {
    sum += vnoise3(x * freq, y * freq, z * freq, seed + i * 7717) * amp;
    norm += amp;
    amp *= 0.5;
    freq *= 2.11;
  }
  return sum / norm;
}
