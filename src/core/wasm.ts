/**
 * ============================================================================
 *  CARREGADOR DO NÚCLEO WASM
 * ============================================================================
 *  O módulo é instanciado N+1 vezes (main thread + N workers) SEMPRE sobre a
 *  MESMA WebAssembly.Memory compartilhada. Cada instância tem seus próprios
 *  globais, mas enxerga os mesmos bytes — é isso que dispensa cópia e
 *  desserialização entre threads.
 *
 *  O binário é minúsculo (< 12 KB) e o instantiateStreaming compila em
 *  paralelo com o download → "carregamento instantâneo".
 */

import type { SharedWorld } from './memory';

export interface Core {
  memory: WebAssembly.Memory;
  /** Bloco de voxels (mesma semântica de mundo do lado JS). */
  getBlockW(hdr: number, x: number, y: number, z: number): number;
  setBlockW(hdr: number, x: number, y: number, z: number, id: number): void;
  isOpaque(id: number): number;
  isSolidForPhysics(id: number): number;
  fillBytes(ptr: number, pattern: number, count: number): void;
  copyBytes(dst: number, src: number, count: number): void;
  /** Geração procedural completa de um chunk (determinística). */
  generateChunk(hdr: number, slot: number, cx: number, cy: number, cz: number): void;
  /** Greedy mesher → escreve cabeçalho + vértices em `outPtr`. Retorna nº de vértices. */
  meshChunk(hdr: number, slot: number, cx: number, cy: number, cz: number,
            outPtr: number, cap: number, scratchPtr: number): number;
  /** Sweep AABB com bissecção. outPtr: [0..2]=pos, [3]=flags. */
  sweepBox(hdr: number, px: number, py: number, pz: number,
           dx: number, dy: number, dz: number,
           hx: number, hy: number, hz: number, outPtr: number): number;
  /** Raycast DDA. outPtr: [0..2]=bloco, [3]=id, [4..6]=normal, [7]=dist. */
  raycastVoxel(hdr: number, ox: number, oy: number, oz: number,
               dx: number, dy: number, dz: number, maxDist: number, outPtr: number): number;
  surfaceY(hdr: number, x: number, z: number): number;
  isSpawnClear(hdr: number, x: number, y: number, z: number): number;
  /** Simulação em lote de corpos rígidos (roda no worker de física). */
  simulateBodies(hdr: number, bodyBase: number, count: number, dt: number, scratch: number): number;
  wakeBodies(bodyBase: number, count: number, x: number, y: number, z: number, radius: number): void;
  coreVersion(): number;
  warmup(hdr: number, scratch: number): number;
}

/** Caminhos dos dois builds do núcleo (ver script `npm run wasm`). */
export const WASM_SHARED = '/wasm/core.wasm';         // threads + shared memory
export const WASM_PLAIN = '/wasm/core-noshared.wasm'; // single-thread seguro

const cache = new Map<string, Promise<ArrayBuffer>>();

function fetchWasm(url: string): Promise<ArrayBuffer> {
  let p = cache.get(url);
  if (!p) {
    p = (async () => {
      const res = await fetch(url, { cache: 'force-cache' });
      if (!res.ok) throw new Error(`Falha ao baixar ${url}: HTTP ${res.status}`);
      return res.arrayBuffer();
    })().catch((err) => { cache.delete(url); throw err; });
    cache.set(url, p);
  }
  return p;
}

/**
 * Instancia o núcleo sobre a memória compartilhada do mundo.
 * @param world  dona da WebAssembly.Memory (importada como env.memory)
 * @param url    caminho do .wasm (default /wasm/core.wasm)
 */
export async function instantiateCore(world: SharedWorld, url?: string): Promise<Core> {
  const src = url ?? (world.isShared ? WASM_SHARED : WASM_PLAIN);
  const bytes = await fetchWasm(src);

  const importObject: WebAssembly.Imports = {
    env: {
      memory: world.memory,
      // O compilador emite `abort` para traps controlados; mantemos o throw
      // para que um erro apareça no console em vez de corromper o heap.
      abort: (msg: number, file: number, line: number, col: number) => {
        throw new Error(`WASM abort @ ${line}:${col} (msg=${msg}, file=${file})`);
      },
    },
  };

  const result = await WebAssembly.instantiate(bytes, importObject);
  const exports = result.instance.exports as unknown as Core & { memory?: WebAssembly.Memory };
  exports.memory = world.memory;

  // Sanidade: o contrato binário é validado em runtime (evita dessincronia
  // entre o .wasm buildado e o layout esperado pelo JS).
  const ver = exports.coreVersion();
  if (ver !== 10007) throw new Error(`Núcleo WASM incompatível: esperado 10007, recebido ${ver}`);

  return exports;
}

/** Instantância "leve" para workers: usa a mesma memória, sem recarregar bytes. */
/** Detecta suporte a SharedArrayBuffer + cross-origin isolation. */
export function hasSharedMemory(): boolean {
  return typeof SharedArrayBuffer !== 'undefined'
      && (self as unknown as { crossOriginIsolated?: boolean }).crossOriginIsolated === true;
}
