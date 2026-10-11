/**
 * ============================================================================
 *  MAPA DE MEMÓRIA COMPARTILHADA — contrato binário JS ↔ WASM ↔ WORKERS
 * ============================================================================
 *  Um único SharedArrayBuffer de 32 MB é a "RAM do jogo". A main thread, o
 *  módulo WASM (importMemory + sharedMemory) e TODOS os workers acessam os
 *  mesmos bytes. Não existe cópia, não existe postMessage de dados: apenas
 *  índices e Atomics.
 *
 *  0x00001000  HEADER       i32[8]  → voxelsBase, tableBase, sizeX/Y/Z, sea, seed, ver
 *  0x00002000  TABLE        i32[640]  slot do chunk (índice linear) ou -1 (não pronto)
 *  0x00003000  CHUNKSTATE   i32[640]  0=livre 1=na fila 2=pronto(gerado+mesh) 3=sujo
 *  0x00004000  CONTROL      i32[256]  índices de tarefa, seq/ack de mesh, heartbeat
 *  0x00008000  TASKS        anel lock-free de tarefas (4096 × 16 B)
 *  0x00020000  BODIES       4096 corpos rígidos × 64 B (lidos pela GPU)
 *  0x00060000  ARENAS       9 arenas de rascunho × 64 KB (uma por worker)
 *  0x00100000  MESHOUT      9 slots de saída do mesher × 512 KB
 *  0x00800000  VOXELS       640 chunks × 32 KB = 20 MB de voxels
 *  ────────────────────────────────────────────────────────────────────────
 *  0x01C00000  fim = 28 MB (512 páginas de 64 KB)
 *
 *  Regras de ouro:
 *   • Toda comunicação entre threads usa Atomics (nunca "escrita solta").
 *   • O worker NUNCA aloca: recebe ponteiros de arena já calculados.
 *   • O render lê voxels/mesh direto daqui (zero desserialização).
 */

import type { Vec3 } from './math';

export const PAGE = 65536;
export const PAGES = 512;                 // 32 MB fixos

// ── Dimensões do mundo (1 chunk = 32³ blocos) ──────────────────────────────
export const W_CHUNKS_X = 12;
export const W_CHUNKS_Y = 4;
export const W_CHUNKS_Z = 12;
export const W_BLOCKS_X = W_CHUNKS_X * 32;   // 384
export const W_BLOCKS_Y = W_CHUNKS_Y * 32;   // 128
export const W_BLOCKS_Z = W_CHUNKS_Z * 32;   // 384
export const SEA_LEVEL = 26;
export const MAX_CHUNKS = W_CHUNKS_X * W_CHUNKS_Y * W_CHUNKS_Z; // 576
export const POOL_SLOTS = 640;
export const CHUNK_VOL = 32768;
export const VERTEX_BYTES = 16;
export const MESH_HDR_BYTES = 64;
export const MAX_WORKERS = 8;
export const BODY_STRIDE = 64;
/** Escala de fixed point para passar floats (posição de edição) por Int32Array. */
export const PHYS_SCALE = 32;
export const MAX_BODIES = 4096;

export const LAYOUT = {
  header: 0x001000,
  table: 0x002000,
  state: 0x003000,
  control: 0x004000,
  tasks: 0x008000,
  bodies: 0x020000,
  arenas: 0x060000,
  meshOut: 0x100000,
  voxels: 0x800000,
  arenaBytes: 64 * 1024,
  /** 640 KB por worker: 64 B de cabeçalho + 2 segmentos de 256 KB. */
  meshSlotBytes: 640 * 1024,
  /** Capacidade de cada segmento (opaco / translúcido) na saída do mesher.
   *  256 KB = 16.384 vértices por segmento *por chunk*. Um chunk de 32³ com
   *  greedy meshing fica em ~600-7000 vértices: margem de 2×-25×. */
  meshSectionCap: 262144,
} as const;

// ── Campos do HEADER ───────────────────────────────────────────────────────
export const H_VOXELS_BASE = 0;
export const H_TABLE_BASE = 1;
export const H_SIZE_X = 2;
export const H_SIZE_Y = 3;
export const H_SIZE_Z = 4;
export const H_SEA = 5;
export const H_SEED = 6;
export const H_VERSION = 7;

// ── Campos do CONTROL ──────────────────────────────────────────────────────
export const C_TASK_HEAD = 0;      // total de tarefas publicadas
export const C_TASK_CLAIM = 1;     // próxima tarefa a reservar (fetch-add)
export const C_TASK_DONE = 2;      // total concluído
export const C_EDIT_EPOCH = 3;     // incrementa a cada edição (acorda física)
export const C_EDIT_X = 4;
export const C_EDIT_Y = 5;
export const C_EDIT_Z = 6;
export const C_PHYS_TICK = 7;      // pedido de tick de física (Atomics.notify)
export const C_PHYS_DONE = 8;      // worker confirma o tick
export const C_AWAKE = 9;          // corpos acordados
export const C_PHYS_DT = 10;       // f32: passo de tempo do próximo tick
export const C_SEQ_BASE = 16;      // 16 + i*2 → seq do mesh do worker i
                                   // 16 + i*2+1 → ack da main thread

// ── Estados de chunk ───────────────────────────────────────────────────────
export const ST_FREE = 0;
export const ST_QUEUED = 1;
export const ST_READY = 2;
export const ST_DIRTY = 3;

// ── Tipos de tarefa ────────────────────────────────────────────────────────
export const JOB_FULL = 0;   // gerar + mesher
export const JOB_REMESH = 1; // só mesher (chunk editado)

/** Índice linear (== slot no pool, pois o mundo cabe inteiro no pool). */
export function chunkIndex(cx: number, cy: number, cz: number): number {
  return (cy * W_CHUNKS_Z + cz) * W_CHUNKS_X + cx;
}
export function chunkCoordFromIndex(i: number): Vec3 {
  const cx = i % W_CHUNKS_X;
  const cz = Math.floor(i / W_CHUNKS_X) % W_CHUNKS_Z;
  const cy = Math.floor(i / (W_CHUNKS_X * W_CHUNKS_Z));
  return [cx, cy, cz];
}
export function isInsideChunkBounds(cx: number, cy: number, cz: number): boolean {
  return cx >= 0 && cy >= 0 && cz >= 0 && cx < W_CHUNKS_X && cy < W_CHUNKS_Y && cz < W_CHUNKS_Z;
}

/**
 * A "RAM do jogo": views tipadas sobre o SAB que a instância WASM importa.
 * A ordem de criação importa — os views são construídos uma vez e reusados.
 */
export class SharedWorld {
  readonly memory: WebAssembly.Memory;
  readonly sab: SharedArrayBuffer;
  readonly i32: Int32Array;
  readonly u32: Uint32Array;
  readonly f32: Float32Array;
  readonly u8: Uint8Array;
  readonly table: Int32Array;
  readonly state: Int32Array;
  readonly control: Int32Array;
  /** Buffer de corpos rígidos (visão tipada por corpo de 64 B). */
  readonly bodies: Float32Array;
  readonly bodiesI32: Int32Array;

  /**
   * @param memory  WebAssembly.Memory compartilhada (criada na main thread e
   *                clonada para os workers via postMessage — structured clone
   *                de Memory é suportado em navegadores modernos).
   * @param initialize  true apenas na main thread: escreve cabeçalho/tabela.
   */
  constructor(memory: WebAssembly.Memory, initialize: boolean, seed: number) {
    this.memory = memory;
    this.sab = memory.buffer as unknown as SharedArrayBuffer;
    this.isShared = this.sab instanceof SharedArrayBuffer;
    this.i32 = new Int32Array(this.sab);
    this.u32 = new Uint32Array(this.sab);
    this.f32 = new Float32Array(this.sab);
    this.u8 = new Uint8Array(this.sab);
    this.table = new Int32Array(this.sab, LAYOUT.table, POOL_SLOTS);
    this.state = new Int32Array(this.sab, LAYOUT.state, POOL_SLOTS);
    this.control = new Int32Array(this.sab, LAYOUT.control, 256);
    this.bodies = new Float32Array(this.sab, LAYOUT.bodies, MAX_BODIES * BODY_STRIDE / 4);
    this.bodiesI32 = new Int32Array(this.sab, LAYOUT.bodies, MAX_BODIES * BODY_STRIDE / 4);

    if (!initialize) return;   // worker: só anexa às views, não reinicializa

    // ── Cabeçalho ────────────────────────────────────────────────────────
    const h = LAYOUT.header >> 2;
    this.i32[h + H_VOXELS_BASE] = LAYOUT.voxels;
    this.i32[h + H_TABLE_BASE] = LAYOUT.table;
    this.i32[h + H_SIZE_X] = W_CHUNKS_X;
    this.i32[h + H_SIZE_Y] = W_CHUNKS_Y;
    this.i32[h + H_SIZE_Z] = W_CHUNKS_Z;
    this.i32[h + H_SEA] = SEA_LEVEL;
    this.i32[h + H_SEED] = seed >>> 0;
    this.i32[h + H_VERSION] = 1;

    this.table.fill(-1);
    this.state.fill(ST_FREE);
    this.control.fill(0);
    // Corpos fora de uso
    for (let i = 0; i < MAX_BODIES; i++) this.bodiesI32[(i * BODY_STRIDE) / 4 + 7] = 0;
  }

  /** A memória é SharedArrayBuffer (multithreading real habilitado)? */
  readonly isShared: boolean = false;

  /**
   * Main thread: cria a memória do mundo e inicializa o cabeçalho.
   *
   * Tenta SEMPRE a memória compartilhada primeiro (é o que permite passar a
   * referência para os workers e compartilhar o heap do WASM). Em ambientes
   * sem COOP/COEP — iframes de terceiros, servidores sem os headers, Safari
   * antigo — a construção lança `TypeError`, e caímos para memória comum:
   * o jogo continua idêntico, apenas single-thread (o núcleo correspondente
   * é o core-noshared.wasm).
   */
  static create(seed: number): SharedWorld {
    let mem: WebAssembly.Memory;
    try {
      mem = new WebAssembly.Memory({ initial: PAGES, maximum: PAGES, shared: true });
    } catch {
      mem = new WebAssembly.Memory({ initial: PAGES, maximum: PAGES });
    }
    return new SharedWorld(mem, true, seed);
  }
  /** Qualquer thread: anexa às mesmas views (memória já existente). */
  static attach(memory: WebAssembly.Memory): SharedWorld {
    return new SharedWorld(memory, false, 0);
  }

  get headerPtr(): number { return LAYOUT.header; }
  get voxelBase(): number { return LAYOUT.voxels; }

  chunkPtr(slot: number): number { return LAYOUT.voxels + slot * CHUNK_VOL; }
  arenaPtr(worker: number): number { return LAYOUT.arenas + worker * LAYOUT.arenaBytes; }
  meshPtr(worker: number): number { return LAYOUT.meshOut + worker * LAYOUT.meshSlotBytes; }

  /** Leitura de voxel (mesma semântica do getBlockW do WASM) em JS. */
  getBlock(x: number, y: number, z: number): number {
    if (y < 0) return 1;
    if (y >= W_BLOCKS_Y) return 0;
    const cx = x >> 5, cy = y >> 5, cz = z >> 5;
    if (cx < 0 || cz < 0 || cx >= W_CHUNKS_X || cz >= W_CHUNKS_Z || cy >= W_CHUNKS_Y) return 1;
    const slot = this.table[chunkIndex(cx, cy, cz)];
    if (slot < 0) return 1;
    return this.u8[LAYOUT.voxels + slot * CHUNK_VOL + ((y & 31) << 10) + ((z & 31) << 5) + (x & 31)];
  }

  /** Escrita direta (a main thread é a única dona da edição do mundo). */
  setBlock(x: number, y: number, z: number, id: number): boolean {
    if (y < 0 || y >= W_BLOCKS_Y) return false;
    const cx = x >> 5, cy = y >> 5, cz = z >> 5;
    if (cx < 0 || cz < 0 || cx >= W_CHUNKS_X || cz >= W_CHUNKS_Z || cy >= W_CHUNKS_Y) return false;
    const idx = chunkIndex(cx, cy, cz);
    const slot = this.table[idx];
    if (slot < 0) return false;
    this.u8[LAYOUT.voxels + slot * CHUNK_VOL + ((y & 31) << 10) + ((z & 31) << 5) + (x & 31)] = id;
    return true;
  }

  // ── Corpos rígidos ──────────────────────────────────────────────────────
  bodyOffset(i: number): number { return (i * BODY_STRIDE) / 4; }
  setBody(i: number, x: number, y: number, z: number, half: number, color: number, vy = 0): void {
    const o = this.bodyOffset(i);
    this.bodies[o + 0] = x; this.bodies[o + 1] = y; this.bodies[o + 2] = z;
    this.bodies[o + 3] = 0; this.bodies[o + 4] = vy; this.bodies[o + 5] = 0;
    this.bodies[o + 6] = half;
    this.bodiesI32[o + 7] = 1;                 // FLAG_ACTIVE
    this.bodiesI32[o + 8] = color >>> 0;       // rgba empacotado
    this.bodies[o + 9] = 0;                    // repouso
  }
  bodyPos(i: number): Vec3 {
    const o = this.bodyOffset(i);
    return [this.bodies[o], this.bodies[o + 1], this.bodies[o + 2]];
  }
  isBodyActive(i: number): boolean {
    return this.bodiesI32[this.bodyOffset(i) + 7] !== 0;
  }
}
