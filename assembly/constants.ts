/**
 * ============================================================================
 *  ROBLOX-MAX · NÚCLEO WASM · CONSTANTES DE LAYOUT
 * ============================================================================
 *  Todo o "contrato binário" entre JS (TS) e WebAssembly vive aqui.
 *  Regra de ouro: potências de 2 → shifts e ANDs, nunca divisão/módulo.
 *
 *  ┌─ CHUNK = 32×32×32 = 32768 bytes (1 byte por voxel, indexado por u8) ─┐
 *  └──────────────────────────────────────────────────────────────────────┘
 *  Indexação linear:  idx = x | (z << 5) | (y << 10)      (x é o eixo rápido)
 *  O eixo rápido importa: o mesher varre slices contíguos de 32 bytes,
 *  o que permite comparar 16 voxels por instrução SIMD (i8x16).
 */

// ───────────────────────── Geometria do chunk ─────────────────────────
export const CHUNK_X: i32 = 32;
export const CHUNK_Y: i32 = 32;
export const CHUNK_Z: i32 = 32;
export const CHUNK_VOL: i32 = 32768; // CHUNK_X*CHUNK_Y*CHUNK_Z
export const CHUNK_MASK: i32 = 31;   // x & 31 == x % 32
export const SH_Y: i32 = 10;         // y << 10 == y * 1024
export const SH_Z: i32 = 5;          // z << 5  == z * 32

// ───────────────────────── Identificadores de bloco ────────────────────
export const BLK_AIR: i32 = 0;
export const BLK_GRASS: i32 = 1;
export const BLK_DIRT: i32 = 2;
export const BLK_STONE: i32 = 3;
export const BLK_SAND: i32 = 4;
export const BLK_WATER: i32 = 5;
export const BLK_LOG: i32 = 6;
export const BLK_LEAVES: i32 = 7;
export const BLK_SNOW: i32 = 8;
export const BLK_PLANK: i32 = 9;
export const BLK_BRICK: i32 = 10;
export const BLK_GLASS: i32 = 11;
export const BLK_COBBLE: i32 = 12;
export const BLK_GRAVEL: i32 = 13;
export const BLK_ICE: i32 = 14;
export const BLK_LAMP: i32 = 15;
export const BLK_BEDROCK: i32 = 16;

// ─────────────── Índice de camada no atlas (texture array 2D) ───────────────
// Espelha src/render/atlas.ts — a ordem tem de ser idêntica.
export const T_GRASS_TOP: i32 = 0;
export const T_GRASS_SIDE: i32 = 1;
export const T_DIRT: i32 = 2;
export const T_STONE: i32 = 3;
export const T_SAND: i32 = 4;
export const T_WATER: i32 = 5;
export const T_LOG_SIDE: i32 = 6;
export const T_LOG_TOP: i32 = 7;
export const T_LEAVES: i32 = 8;
export const T_SNOW: i32 = 9;
export const T_PLANK: i32 = 10;
export const T_BRICK: i32 = 11;
export const T_GLASS: i32 = 12;
export const T_COBBLE: i32 = 13;
export const T_GRAVEL: i32 = 14;
export const T_ICE: i32 = 15;
export const T_LAMP: i32 = 16;
export const T_BEDROCK: i32 = 17;
export const ATLAS_LAYERS: i32 = 18;

// ─────────────────── Direções das faces (ordem canônica) ───────────────────
// 0:+X  1:-X  2:+Y  3:-Y  4:+Z  5:-Z   (usada no vetor normal do vértice)
export const FACE_PX: i32 = 0;
export const FACE_NX: i32 = 1;
export const FACE_PY: i32 = 2;
export const FACE_NY: i32 = 3;
export const FACE_PZ: i32 = 4;
export const FACE_NZ: i32 = 5;

// ───────────────────────── Formato do vértice (16 B) ──────────────────────
// Word A (u32) : x (u16) | y (u16)
// Word B (u32) : z (u16) | normal (u3 << 16) | ao (u2 << 19) | light (u4 << 21)
// Word C (u32) : u (u8) | v (u8) | layer (u16)
// Word D (u32) : sky (u8) | blockLight (u8) | tint (u8) | reserved (u8)
export const VERTEX_BYTES: i32 = 16;
export const VERTS_PER_QUAD: i32 = 6; // 2 triângulos não-indexados (zero gestão de index buffer)

// ─────────────────── Cabeçalho de saída do mesh (por slot) ────────────────
export const MH_OPAQUE_VERTS: i32 = 0;  // u32
export const MH_WATER_VERTS: i32 = 4;   // u32
export const MH_STATUS: i32 = 8;        // u32 (bit0 = truncado)
export const MH_QUADS: i32 = 12;        // u32
export const MH_MIN_X: i32 = 16;        // f32 ×6 (AABB em coords de mundo)
export const MH_MAX_X: i32 = 28;
export const MESH_HDR_BYTES: i32 = 64;  // dados começam em outPtr + 64

// ──────────────── Estrutura de corpo rígido dinâmico (64 B) ───────────────
// Mantida em memória compartilhada: worker de física escreve, main thread lê
// (instanced draw) sem cópia nem postMessage.
export const BODY_STRIDE: i32 = 64;
export const B_PX: i32 = 0;
export const B_PY: i32 = 4;
export const B_PZ: i32 = 8;
export const B_VX: i32 = 12;
export const B_VY: i32 = 16;
export const B_VZ: i32 = 20;
export const B_HALF: i32 = 24;   // meio-tamanho do cubo
export const B_FLAGS: i32 = 28;  // bit0 active, bit1 grounded, bit2 sleeping
export const B_COLOR: i32 = 32;  // rgba empacotado
export const B_REST: i32 = 36;   // tempo em repouso (ms) → sleeping

export const FLAG_ACTIVE: i32 = 1;
export const FLAG_GROUNDED: i32 = 2;
export const FLAG_SLEEPING: i32 = 4;

// ───────────────────────── Física: constantes de gameplay ─────────────────
export const GRAVITY: f32 = 32.0;        // blocos/s²
export const TERMINAL_VEL: f32 = 60.0;
export const WATER_DRAG: f32 = 0.86;
export const AIR_DRAG: f32 = 0.995;
export const MAX_SUBSTEP: f32 = 0.45;    // nunca atravessa mais que ~meio bloco/step
