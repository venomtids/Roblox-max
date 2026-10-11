/**
 * ============================================================================
 *  PONTO DE ENTRADA DO MÓDULO WASM
 * ============================================================================
 *  O binário é compilado com:
 *      asc assembly/index.ts -o public/wasm/core.wasm \
 *        --optimize --shrinkLevel 1 --noAssert \
 *        --enable simd --enable bulk-memory \
 *        --importMemory --sharedMemory --maximumMemory 768 \
 *        --runtime stub --exportRuntime
 *
 *  Consequências do design (e por que ele é rápido):
 *   • `--importMemory --sharedMemory`: TODAS as instâncias (main thread +
 *     N workers) compartilham o MESMO linear memory. Um chunk gerado no
 *     worker #3 é lido pela física e pelo render sem cópia.
 *   • `--runtime stub`: sem GC, sem TLSF, sem alocação dinâmica. Todo vetor
 *     é um ponteiro de arena calculado pela main thread.
 *   • `--enable simd`: kernels em 16 bytes/instrução (i8x16/v128).
 *   • ZERO estado mutável em variável de módulo: todo contexto entra por
 *     parâmetro (worldPtr/arena), então instâncias são stateless e thread-safe.
 */

import * as W from "./world";
import * as G from "./generate";
import * as M from "./mesh";
import * as P from "./physics";
import * as S from "./sim";
import * as N from "./noise";

// ── Mundo e voxels ────────────────────────────────────────────────────────
export { getBlockW, setBlockW, isOpaque, isSolidForPhysics, fillBytes, copyBytes } from "./world";
export { generateChunk } from "./generate";
export { meshChunk } from "./mesh";

// ── Física ────────────────────────────────────────────────────────────────
export { sweepBox, raycastVoxel, surfaceY, isSpawnClear } from "./physics";
export { simulateBodies, wakeBodies } from "./sim";

// ── Utilidades expostas para a main thread (evitam duplicar lógica em JS) ──
/** Versão do módulo — a main thread valida contra src/world/constants.ts. */
export function coreVersion(): i32 { return 10007; }

/**
 * Anti-travamento: assegura que o mundo "toque" o benchmark também com
 * memória fria (primeira execução). Chama um pouco de tudo uma vez.
 */
export function warmup(hdr: i32, scratch: usize): i32 {
  const n: f32 = N.fbm2(1.5, 2.5, 3, 1234) + N.fbm3(0.5, 0.5, 0.5, 2, 99);
  W.fillBytes(scratch, 1, 256);
  return i32(n * 1000.0);
}
