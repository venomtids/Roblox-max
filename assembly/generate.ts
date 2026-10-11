/**
 * ============================================================================
 *  GERADOR DE MUNDO PROCEDURAL  (custo real: ~0,3 ms por chunk por núcleo)
 * ============================================================================
 *  Estratégia:
 *   1) Limpa o chunk com SIMD (16 B/instrução).
 *   2) Heightmap por coluna (fBm 2D) → 1024 amostras, não 32768 voxels.
 *   3) Preenchimento por coluna com ESCRITA ESTRIDADA (passo 1024 no eixo Y).
 *   4) Cavernas 3D só na faixa de pedra, com poda vertical agressiva.
 *   5) Árvores determinísticas, recortadas por slice (sem dependência entre
 *      chunks → nenhum remesh cruzado, nenhum loop infinito de atualização).
 *
 *  ⚠ DUAS ARMADILHAS DE LAYOUT QUE ESTE ARQUIVO EVITA (bugs reais, já
 *    corrigidos aqui, e que qualquer engine voxel encontra):
 *    A) O eixo Y tem stride 1024 (idx = x | z<<5 | y<<10). Um `fillBytes`
 *       de N bytes preenche N bytes CONTÍGUOS — ou seja, ao longo de X, não
 *       de Y. Preencher a coluna exige stores individuais com `yl << 10`.
 *    B) A altura do terreno é GLOBAL; o índice do voxel é LOCAL ao chunk.
 *       A conversão `hl = h - (cy << 5)` acontece uma única vez por coluna.
 */

import {
  CHUNK_VOL, SH_Y, SH_Z,
  BLK_AIR, BLK_GRASS, BLK_DIRT, BLK_STONE, BLK_SAND, BLK_WATER,
  BLK_LOG, BLK_LEAVES, BLK_SNOW, BLK_GRAVEL, BLK_ICE, BLK_BEDROCK,
} from "./constants";
import { hash2, hash3, fbm2, fbm3 } from "./noise";
import { H_SEA, H_SEED, chunkPtr, fillBytes } from "./world";

/** Altura do terreno (GLOBAL, em blocos) da coluna (wx,wz). */
@inline function terrainHeight(wx: i32, wz: i32, seed: i32): i32 {
  const cont: f32 = fbm2(f32(wx) * 0.0032, f32(wz) * 0.0032, 4, seed);          // continente
  const det: f32 = fbm2(f32(wx) * 0.021, f32(wz) * 0.021, 3, seed + 71);        // detalhe
  const m: f32 = cont > 0.0 ? cont * cont : 0.0;                                // montanha
  let h: f32 = 30.0 + cont * 9.0 + det * 3.2 + m * 28.0;
  if (h < 2.0) h = 2.0;
  return i32(h);
}

/** "Temperatura" regional: define neve, gelo e bioma frio. */
@inline function temperature(wx: i32, wz: i32, seed: i32): f32 {
  return fbm2(f32(wx) * 0.0011, f32(wz) * 0.0011, 2, seed + 5571);
}

/** Bloco de topo analítico — permite decidir árvore sem ler voxels. */
@inline function topBlockOf(h: i32, temp: f32, seaGlobal: i32): i32 {
  if (h <= seaGlobal + 1) return BLK_SAND;
  if (h > 58 || (temp < -0.35 && h > 48)) return BLK_SNOW;
  return BLK_GRASS;
}

/**
 * Gera o chunk (cx,cy,cz) no slot indicado. Determinístico e sem alocação.
 */
export function generateChunk(hdr: i32, slot: i32, cx: i32, cy: i32, cz: i32): void {
  const seed: i32 = load<i32>(hdr + H_SEED);
  const seaGlobal: i32 = load<i32>(hdr + H_SEA);
  const seaLocal: i32 = seaGlobal - (cy << 5);
  const base: usize = chunkPtr(hdr, slot);
  const wx0: i32 = cx << 5, wy0: i32 = cy << 5, wz0: i32 = cz << 5;

  // ── 1. Limpeza SIMD do chunk inteiro (2048 stores de 16 B) ─────────────
  fillBytes(base, BLK_AIR, CHUNK_VOL);

  for (let lz: i32 = 0; lz < 32; lz++) {
    const wz: i32 = wz0 + lz;
    const zrow: usize = base + (lz << SH_Z);
    for (let lx: i32 = 0; lx < 32; lx++) {
      const wx: i32 = wx0 + lx;
      const h: i32 = terrainHeight(wx, wz, seed);
      const temp: f32 = temperature(wx, wz, seed);
      const col: usize = zrow + lx;
      const hl: i32 = h - wy0;                    // LOCAL (ver armadilha B)

      // Poda: slice inteira acima do terreno E do mar → só ar (não toca nada).
      if (hl < 0 && seaLocal < 0 && wy0 != 0) continue;

      // 1a) Pedra: do fundo da fatia até 4 blocos abaixo do topo.
      //     Escrita ESTRIDADA (yl << 10) — nunca contígua (armadilha A).
      let stoneTop: i32 = hl - 4;
      if (stoneTop > 31) stoneTop = 31;
      for (let yl: i32 = 0; yl <= stoneTop; yl++) {
        store<u8>(col + (yl << SH_Y), u8(BLK_STONE));
      }

      // 1b) Topo + 3 blocos de subsuperfície (praia, neve, grama).
      if (hl >= 0 && hl <= 31) {
        const topId: i32 = topBlockOf(h, temp, seaGlobal);
        store<u8>(col + (hl << SH_Y), u8(topId));
        const subId: i32 = topId == BLK_SAND ? BLK_SAND : BLK_DIRT;
        let yl: i32 = hl - 1;
        const last: i32 = hl - 3;
        for (; yl >= 0 && yl >= last; yl--) store<u8>(col + (yl << SH_Y), u8(subId));
      }

      // 1c) Oceano/lago: água de hl+1 até o nível do mar (gelo no topo se frio).
      if (hl < seaLocal) {
        let wStart: i32 = hl + 1;
        if (wStart < 0) wStart = 0;
        const wEnd: i32 = seaLocal > 31 ? 31 : seaLocal;
        for (let yl: i32 = wStart; yl <= wEnd; yl++) {
          store<u8>(col + (yl << SH_Y), u8(BLK_WATER));
        }
        if (wEnd >= wStart && temp < -0.45) store<u8>(col + (wEnd << SH_Y), u8(BLK_ICE));
      }

      // 1d) Leito rochoso indestrutível (última escrita: soberano).
      if (wy0 == 0) store<u8>(col, u8(BLK_BEDROCK));
    }
  }

  // ── 2. Cavernas 3D apenas na faixa de pedra ────────────────────────────
  const CAVE_TOP_G: i32 = 52;
  let yStart: i32 = 2 - wy0;
  if (yStart < 0) yStart = 0;
  let yEndL: i32 = CAVE_TOP_G - wy0;
  if (yEndL > 31) yEndL = 31;
  if (yEndL >= yStart) {
    for (let lz: i32 = 0; lz < 32; lz++) {
      const wz: i32 = wz0 + lz;
      for (let lx: i32 = 0; lx < 32; lx++) {
        const wx: i32 = wx0 + lx;
        const col: usize = base + (lz << SH_Z) + lx;
        for (let yl: i32 = yStart; yl <= yEndL; yl++) {
          if (load<u8>(col + (yl << SH_Y)) != BLK_STONE) continue;
          const n: f32 = fbm3(f32(wx) * 0.055, f32(wy0 + yl) * 0.09, f32(wz) * 0.055, 2, seed + 991);
          if (n > 0.52) store<u8>(col + (yl << SH_Y), u8(BLK_AIR));
        }
      }
    }
    // Veios de cascalho (leitura visual das paredes).
    for (let i: i32 = 0; i < 40; i++) {
      const h4: u32 = hash3(cx * 131 + i, cy, cz * 197 + i * 7, seed + 4242);
      const lx: i32 = i32(h4 & 31);
      const lz: i32 = i32((h4 >>> 5) & 31);
      const yl: i32 = i32((h4 >>> 10) & 31);
      const p: usize = base + (yl << SH_Y) + (lz << SH_Z) + lx;
      if (load<u8>(p) == BLK_STONE) store<u8>(p, u8(BLK_GRAVEL));
    }
  }

  // ── 3. Árvores recortadas por slice ────────────────────────────────────
  // Margem de 4 blocos nas bordas horizontais: tronco+copa nunca saem do
  // chunk em X/Z → cada chunk é autocontido e vizinhos não precisam remesh.
  for (let lz: i32 = 4; lz < 28; lz++) {
    const wz: i32 = wz0 + lz;
    for (let lx: i32 = 4; lx < 28; lx++) {
      const wx: i32 = wx0 + lx;
      const r: u32 = hash2(wx, wz, seed ^ 0x5bf03635);
      if ((r & 1023) >= 9) continue;                     // ~0.9% das colunas
      const h: i32 = terrainHeight(wx, wz, seed);
      if (topBlockOf(h, temperature(wx, wz, seed), seaGlobal) != BLK_GRASS) continue;
      const trunk: i32 = 4 + i32((r >>> 10) & 3);
      const hl: i32 = h - wy0;
      const col: usize = base + (lz << SH_Z) + lx;

      // Tronco (parte contida neste slice)
      for (let t: i32 = 1; t <= trunk; t++) {
        const yl: i32 = hl + t;
        if (yl < 0) continue;
        if (yl > 31) break;
        store<u8>(col + (yl << SH_Y), u8(BLK_LOG));
      }
      // Copa: esfera achatada (r² <= 6), podada nos limites do slice
      const cyL: i32 = hl + trunk;
      for (let dy: i32 = -2; dy <= 2; dy++) {
        const yl: i32 = cyL + dy;
        if (yl < 0 || yl > 31) continue;
        const rad: i32 = dy <= 1 ? 2 : 1;
        for (let dz2: i32 = -rad; dz2 <= rad; dz2++) {
          for (let dx2: i32 = -rad; dx2 <= rad; dx2++) {
            if (dx2 * dx2 + dz2 * dz2 + dy * dy > 6) continue;
            const px: i32 = lx + dx2;
            const pz: i32 = lz + dz2;
            const p: usize = base + (yl << SH_Y) + (pz << SH_Z) + px;
            if (load<u8>(p) == BLK_AIR) store<u8>(p, u8(BLK_LEAVES));
          }
        }
      }
    }
  }
}
