/**
 * ============================================================================
 *  JOB DE CHUNK — o "trabalho grosso" do engine, em uma função só
 * ============================================================================
 *  Esta função é importada IDENTICAMENTE pela main thread e pelos workers.
 *  Como todos compartilham o mesmo linear memory do WASM, o trabalho pode
 *  migrar entre threads sem nenhuma mudança de código ou de dados.
 */

import type { Core } from '../core/wasm';
import { SharedWorld, chunkIndex, ST_READY, LAYOUT, MESH_HDR_BYTES } from '../core/memory';
import { JOB_FULL, type Task } from './tasks';

export interface JobResult {
  slot: number;
  verts: number;
  quads: number;
  truncated: boolean;
  genMs: number;
  meshMs: number;
  aabb: Float32Array;   // [minx,miny,minz,maxx,maxy,maxz] em blocos de mundo
}

const scratchAabb = new Float32Array(6);

/**
 * Executa uma tarefa de chunk no worker `workerId` (arena + slot de mesh).
 * Fluxo: (opcional) gerar voxels → greedy mesh → publica seq no canal.
 *
 * @returns resultado com estatísticas (para o HUD/profiler)
 */
export function runChunkJob(
  core: Core, world: SharedWorld, task: Task, workerId: number,
  channel: { workerPublish(i: number): void; workerWaitForAck(i: number, t?: number): void },
): JobResult {
  const hdr = world.headerPtr;
  const slot = chunkIndex(task.cx, task.cy, task.cz);
  const outPtr = world.meshPtr(workerId);
  const scratch = world.arenaPtr(workerId);

  let genMs = 0;
  if (task.kind === JOB_FULL) {
    const t0 = performance.now();
    core.generateChunk(hdr, slot, task.cx, task.cy, task.cz);
    genMs = performance.now() - t0;
  }

  // Espera a main thread liberar o slot de saída (upload concluído).
  channel.workerWaitForAck(workerId);

  const t1 = performance.now();
  // cap = 64 (cabeçalho) + 2 × 256 KB de segmentos → seção idêntica ao layout
  // de GPU usado pelo renderer (upload em dois ranges exatos).
  const cap = MESH_HDR_BYTES + 2 * LAYOUT.meshSectionCap;
  const verts = core.meshChunk(hdr, slot, task.cx, task.cy, task.cz, outPtr, cap, scratch);
  const meshMs = performance.now() - t1;

  // AABB escrita pelo mesher no cabeçalho (offsets 16 e 28, já em coords mundo)
  const h32 = new Float32Array(world.sab, outPtr + 16, 6);
  for (let i = 0; i < 6; i++) scratchAabb[i] = h32[i];

  Atomics.store(world.table, slot, slot);
  Atomics.store(world.state, slot, ST_READY);
  Atomics.notify(world.state, slot);
  channel.workerPublish(workerId);

  return {
    slot,
    verts,
    quads: new Uint32Array(world.sab, outPtr + 12, 1)[0],
    truncated: new Uint32Array(world.sab, outPtr + 8, 1)[0] !== 0,
    genMs,
    meshMs,
    aabb: scratchAabb,
  };
}
