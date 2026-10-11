/**
 * ============================================================================
 *  ATLAS PROCEDURAL — textura array 32×32 × 18 layers, gerada no cliente
 * ============================================================================
 *  Por que gerar em vez de baixar um PNG?
 *   • Zero rede: o jogo abre instantaneamente (nenhum KB de asset de textura).
 *   • Formato ideal: textura-array (sem bleeding entre tiles, sem atlas math).
 *   • ARGB "pixel art" gerado por ruído hash determinístico → estilo coeso.
 *
 *  O índice das camadas é um CONTRATO com assembly/constants.ts (T_*).
 */

import type { Core } from '../core/wasm';

export const TILE = 32;
export const LAYERS = 18;

/** PRNG determinístico (xorshift) para o pixel art. */
function rng(seed: number): () => number {
  let s = seed >>> 0 || 1;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s / 4294967296;
  };
}

interface Rgb { r: number; g: number; b: number; a?: number }

function putTile(data: Uint8Array, layer: number, fn: (x: number, y: number, r: () => number) => Rgb, seed: number): void {
  const r = rng(seed);
  const base = layer * TILE * TILE * 4;
  for (let y = 0; y < TILE; y++) {
    for (let x = 0; x < TILE; x++) {
      const c = fn(x, y, r);
      const i = base + (y * TILE + x) * 4;
      data[i] = c.r; data[i + 1] = c.g; data[i + 2] = c.b; data[i + 3] = c.a ?? 255;
    }
  }
}

const clamp255 = (v: number) => (v < 0 ? 0 : v > 255 ? 255 : v | 0);

/** Gera as 18 camadas em RGBA8 (32×32 cada) — ~74 KB. */
export function buildAtlas(): Uint8Array {
  const data = new Uint8Array(LAYERS * TILE * TILE * 4);

  // 0 · grama (topo): verde com variação + tufos
  putTile(data, 0, (x, y, r) => {
    const n = r() * 26;
    const tuft = ((x * 7 + y * 13) % 11 === 0) ? 20 : 0;
    return { r: 72 + n + tuft, g: 148 + n + tuft, b: 62 + n * 0.5 };
  }, 101);

  // 1 · grama (lado): terra com faixa verde no topo
  putTile(data, 1, (x, y, r) => {
    if (y < 5 + ((x * 5) % 2)) {
      const n = r() * 22;
      return { r: 70 + n, g: 146 + n, b: 60 + n * 0.5 };
    }
    const n = r() * 24;
    return { r: 124 + n, g: 88 + n * 0.6, b: 58 + n * 0.4 };
  }, 102);

  // 2 · terra
  putTile(data, 2, (_x, _y, r) => { const n = r() * 26; return { r: 126 + n, g: 90 + n * 0.6, b: 58 + n * 0.4 }; }, 103);

  // 3 · pedra: cinza com "rachaduras" lineares
  putTile(data, 3, (x, y, r) => {
    const crack = ((x * 3 + y * 5) % 17 === 0) ? -26 : 0;
    const n = r() * 20;
    return { r: 126 + n + crack, g: 126 + n + crack, b: 132 + n + crack };
  }, 104);

  // 4 · areia
  putTile(data, 4, (_x, _y, r) => { const n = r() * 22; return { r: 224 + n * 0.3, g: 208 + n * 0.35, b: 150 + n * 0.4 }; }, 105);

  // 5 · água: azul translúcido com ondulação
  putTile(data, 5, (x, y, r) => {
    const w = Math.sin((x + y * 0.6) * 0.55) * 10;
    const n = r() * 10;
    return { r: 40 + w * 0.4 + n, g: 110 + w + n, b: 190 + w + n, a: 205 };
  }, 106);

  // 6 · tronco (lado): estrias verticais
  putTile(data, 6, (x, _y, r) => {
    const stripe = (x % 7 === 0 || x % 11 === 0) ? -22 : 0;
    const n = r() * 16;
    return { r: 108 + n + stripe, g: 76 + n * 0.7 + stripe, b: 46 + n * 0.5 + stripe };
  }, 107);

  // 7 · tronco (topo): anéis de crescimento
  putTile(data, 7, (x, y, r) => {
    const d = Math.hypot(x - 15.5, y - 15.5);
    const ring = (Math.sin(d * 1.35) > 0 ? 16 : -10);
    const n = r() * 12;
    return { r: 146 + n + ring, g: 108 + n + ring, b: 68 + n + ring };
  }, 108);

  // 8 · folhas: verde escuro com "buracos" (alpha test)
  putTile(data, 8, (x, y, r) => {
    const hole = ((x * 13 + y * 7) % 19 === 0) || r() < 0.06;
    const n = r() * 30;
    return { r: 46 + n, g: 108 + n, b: 44 + n * 0.6, a: hole ? 0 : 255 };
  }, 109);

  // 9 · neve
  putTile(data, 9, (_x, _y, r) => { const n = r() * 12; return { r: 238 + n * 0.5, g: 244 + n * 0.4, b: 252 + n * 0.2 }; }, 110);

  // 10 · madeira trabalhada
  putTile(data, 10, (x, y, r) => {
    const plank = ((y % 8 === 0) ? -30 : 0) + ((x * 2 + y) % 23 === 0 ? -12 : 0);
    const n = r() * 14;
    return { r: 168 + n + plank, g: 124 + n + plank, b: 74 + n + plank };
  }, 111);

  // 11 · tijolo
  putTile(data, 11, (x, y, r) => {
    const row = Math.floor(y / 8);
    const off = row % 2 ? 8 : 0;
    const mortar = (y % 8 === 0) || ((x + off) % 16 === 0);
    const n = r() * 12;
    return mortar ? { r: 186 + n, g: 178 + n, b: 170 + n }
                  : { r: 152 + n, g: 64 + n * 0.6, b: 52 + n * 0.5 };
  }, 112);

  // 12 · vidro: quase transparente com moldura
  putTile(data, 12, (x, y) => {
    const edge = x === 0 || y === 0 || x === 31 || y === 31;
    const shine = (x + y) % 13 === 0;
    return { r: 216, g: 240, b: 252, a: edge ? 130 : (shine ? 70 : 26) };
  }, 113);

  // 13 · pedregulho
  putTile(data, 13, (x, y, r) => {
    const cell = ((Math.floor(x / 5) * 3 + Math.floor(y / 5) * 7) % 5 === 0) ? -24 : 0;
    const n = r() * 24;
    return { r: 128 + n + cell, g: 126 + n + cell, b: 124 + n + cell };
  }, 114);

  // 14 · cascalho
  putTile(data, 14, (_x, _y, r) => { const n = r() * 46; return { r: 130 + n, g: 122 + n * 0.9, b: 112 + n * 0.85 }; }, 115);

  // 15 · gelo: azul-claro translúcido com fraturas
  putTile(data, 15, (x, y, r) => {
    const frac = ((x * 5 + y * 11) % 29 === 0) ? 34 : 0;
    const n = r() * 12;
    return { r: 158 + n + frac, g: 206 + n + frac, b: 236 + n, a: 190 };
  }, 116);

  // 16 · lâmpada (emissiva)
  putTile(data, 16, (x, y, r) => {
    const d = Math.hypot(x - 15.5, y - 15.5);
    const glow = Math.max(0, 1 - d / 16) * 60;
    const n = r() * 8;
    return { r: clamp255(250 + n), g: clamp255(216 + glow + n), b: clamp255(120 + glow * 0.6 + n) };
  }, 117);

  // 17 · rocha-matriz (bedrock)
  putTile(data, 17, (x, y, r) => {
    const cell = ((Math.floor(x / 8) * 5 + Math.floor(y / 8) * 3) % 7 === 0) ? -18 : 0;
    const n = r() * 16;
    return { r: 62 + n + cell, g: 62 + n + cell, b: 70 + n + cell };
  }, 118);

  return data;
}

/** Sobe o atlas para uma textura-array da GPU (18 camadas, nearest-neighbor). */
export function createAtlasTexture(device: GPUDevice, data: Uint8Array): GPUTexture {
  const tex = device.createTexture({
    size: [TILE, TILE, LAYERS],
    dimension: '2d',
    format: 'rgba8unorm',
    usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
  });
  const bytesPerRow = TILE * 4;
  for (let layer = 0; layer < LAYERS; layer++) {
    const src = data.subarray(layer * TILE * TILE * 4, (layer + 1) * TILE * TILE * 4);
    device.queue.writeTexture(
      { texture: tex, origin: { x: 0, y: 0, z: layer } },
      // A API aceita [AllowShared] BufferSource; a tipagem do TS é mais estreita.
      src as unknown as GPUAllowSharedBufferSource,
      { bytesPerRow, rowsPerImage: TILE }, [TILE, TILE, 1],
    );
  }
  return tex;
}

/** Usa o núcleo WASM (SIMD) se disponível para variar tons em massa. */
export function atlasChecksum(core: Core, data: Uint8Array): number {
  let acc = 0;
  for (let i = 0; i < data.length; i += 97) acc = (acc * 31 + data[i]) >>> 0;
  return (acc ^ core.coreVersion()) >>> 0;
}
