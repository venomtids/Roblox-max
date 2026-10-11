/**
 * ============================================================================
 *  ACESSO AO MUNDO (tabela de chunks + pool de voxels, tudo em shared memory)
 * ============================================================================
 *  Mapa de memória (mesmos offsets em src/world/memory.ts — contrato fixo):
 *
 *  0x00000000  HEADER      struct WorldHeader (i32 × 8)
 *  0x00001000  TABLE       i32[SLOTS] → slot do chunk ou (-slot-1)=pendente, -1=vazio
 *  0x00002000  TASKS       anel de tarefas lock-free (Atomics)
 *  0x00020000  BODIES      corpos rígidos dinâmicos (BODY_STRIDE=64)
 *  0x00080000  ARENAS      scratch por worker (64 KB × MAX_WORKERS)
 *  0x00180000  MESHOUT     4 slots × 512 KB por worker
 *  0x01180000  VOXELS      512 slots × 32 KB de voxels = 16 MB
 *
 *  WorldHeader:
 *    [0] voxelsBase   [1] tableBase   [2] sizeX   [3] sizeY
 *    [4] sizeZ        [5] seaLevel    [6] seed    [7] flags/versão
 */

import {
  CHUNK_X, CHUNK_Y, CHUNK_Z, CHUNK_VOL, CHUNK_MASK, SH_Y, SH_Z,
  BLK_AIR, BLK_WATER, BLK_GLASS, BLK_LEAVES, BLK_ICE, BLK_AIR as AIR,
} from "./constants";

export const H_VOXELS_BASE: i32 = 0;
export const H_TABLE_BASE: i32 = 4;
export const H_SIZE_X: i32 = 8;
export const H_SIZE_Y: i32 = 12;
export const H_SIZE_Z: i32 = 16;
export const H_SEA: i32 = 20;
export const H_SEED: i32 = 24;
export const H_FLAGS: i32 = 28;
export const H_BYTES: i32 = 64;

/** Slot do chunk (cx,cy,cz) ou -2 se fora do mundo. -1 = vazio, <-1 = pendente. */
@inline export function slotOf(hdr: i32, cx: i32, cy: i32, cz: i32): i32 {
  const sx: i32 = load<i32>(hdr + H_SIZE_X);
  const sy: i32 = load<i32>(hdr + H_SIZE_Y);
  const sz: i32 = load<i32>(hdr + H_SIZE_Z);
  if (u32(cx) >= u32(sx) || u32(cy) >= u32(sy) || u32(cz) >= u32(sz)) return -2;
  const idx: i32 = (cy * sz + cz) * sx + cx;
  return load<i32>(load<i32>(hdr + H_TABLE_BASE) + idx * 4);
}

/** Ponteiro direto para os 32 KB de voxels de um slot. */
@inline export function chunkPtr(hdr: i32, slot: i32): usize {
  return load<i32>(hdr + H_VOXELS_BASE) + slot * CHUNK_VOL;
}

/**
 * Leitura de voxel em coordenadas de MUNDO.
 * Semântica de borda (decisão de projeto, não bug):
 *   • acima do mundo (y >= sizeY*32) → AR (permite ver o céu)
 *   • abaixo de y=0 → SÓLIDO (nunca cai do mundo)
 *   • chunk não carregado / pendente / fora do mundo → SÓLIDO
 *     (oclusão conservadora: evita buracos e quedas durante o streaming)
 */
export function getBlockW(hdr: i32, x: i32, y: i32, z: i32): i32 {
  if (y < 0) return 1;
  const maxY: i32 = load<i32>(hdr + H_SIZE_Y) << 5;
  if (y >= maxY) return BLK_AIR;
  const cx: i32 = x >> 5; const cy: i32 = y >> 5; const cz: i32 = z >> 5;
  const s: i32 = slotOf(hdr, cx, cy, cz);
  if (s < 0) return 1;
  return load<u8>(chunkPtr(hdr, s) + ((y & CHUNK_MASK) << SH_Y) + ((z & CHUNK_MASK) << SH_Z) + (x & CHUNK_MASK));
}

/**
 * Leitura com fast-path local: se o voxel pertence ao chunk já carregado em
 * `dataPtr`, lê direto sem consultar a tabela (3 ops em vez de ~15).
 * Caminho quentíssimo do mesher — chamado ~400k vezes por chunk.
 */
@inline export function getBlockFast(hdr: i32, dataPtr: usize, cx: i32, cy: i32, cz: i32, x: i32, y: i32, z: i32): i32 {
  const lx: i32 = x - (cx << 5); const ly: i32 = y - (cy << 5); const lz: i32 = z - (cz << 5);
  if (u32(lx) < 32 && u32(ly) < 32 && u32(lz) < 32) {
    return load<u8>(dataPtr + (ly << SH_Y) + (lz << SH_Z) + lx);
  }
  return getBlockW(hdr, x, y, z);
}

/** Escrita de voxel em coordenadas de mundo (usada por edições e pelo gerador). */
export function setBlockW(hdr: i32, x: i32, y: i32, z: i32, id: i32): void {
  const s: i32 = slotOf(hdr, x >> 5, y >> 5, z >> 5);
  if (s < 0) return;
  store<u8>(chunkPtr(hdr, s) + ((y & CHUNK_MASK) << SH_Y) + ((z & CHUNK_MASK) << SH_Z) + (x & CHUNK_MASK), u8(id));
}

/** Um bloco é opaco (bloqueia visão/luz/corpo)? */
@inline export function isOpaque(id: i32): bool {
  // Vidro, água e gelo são translúcidos; folhas são opacas com buracos (alpha test).
  return id != BLK_AIR && id != BLK_WATER && id != BLK_GLASS && id != BLK_ICE;
}

/** Um bloco é do "estágio translúcido" (passa pelo blend, ordenado)? */
@inline export function isTransparentStage(id: i32): bool {
  return id == BLK_WATER || id == BLK_GLASS || id == BLK_ICE;
}

/** Sólido para colisão de corpos? (água, vidro e folhas não barram... folhas barram) */
@inline export function isSolidForPhysics(id: i32): bool {
  return id != BLK_AIR && id != BLK_WATER;
}

/**
 * SIMD: preenche `count` bytes com o padrão `pattern` (ex.: stone).
 * 16 bytes por instrução — usado ao encher colunas inteiras de terreno.
 */
export function fillBytes(ptr: usize, pattern: i32, count: i32): void {
  const v = i8x16.splat(i8(pattern));
  let i: i32 = 0;
  const n16: i32 = count & ~15;
  for (; i < n16; i += 16) {
    v128.store(ptr + i, v);
  }
  for (; i < count; i++) store<u8>(ptr + i, u8(pattern));
}

/** SIMD: memcpy de 32 KB (usado ao copiar chunk base → slot de trabalho). */
export function copyBytes(dst: usize, src: usize, count: i32): void {
  let i: i32 = 0;
  const n16: i32 = count & ~15;
  for (; i < n16; i += 16) v128.store(dst + i, v128.load(src + i));
  for (; i < count; i++) store<u8>(dst + i, load<u8>(src + i));
}
