/// <reference lib="webworker" />
/**
 * ============================================================================
 *  WORKER DE CHUNKS — geração procedural + greedy meshing em paralelo
 * ============================================================================
 *  Cada worker:
 *    • importa a MESMA WebAssembly.Memory compartilhada (structured clone);
 *    • instancia o núcleo WASM (barato: < 1 ms) sobre a memória já existente;
 *    • consome o anel de tarefas lock-free (Atomics) sem nenhum lock;
 *    • escreve o mesh na SUA área de saída e publica um número de sequência;
 *    • entra em `Atomics.wait` quando não há trabalho → 0% de CPU ocioso.
 *
 *  REGRA DE OURO: o worker nunca envia dados geométricos por postMessage.
 *  Ele envia um índice; a main thread lê os vértices direto da memória.
 */

import { SharedWorld, MAX_WORKERS } from '../core/memory';
import { instantiateCore } from '../core/wasm';
import { TaskRing, MeshChannel } from '../world/tasks';
import { runChunkJob } from '../world/jobs';

interface InitMsg {
  type: 'init';
  memory: WebAssembly.Memory;
  wasmUrl: string;
  workerId: number;
}

let world: SharedWorld | null = null;
let ring: TaskRing | null = null;
let channel: MeshChannel | null = null;
let workerId = 0;
let running = false;

self.onmessage = async (ev: MessageEvent) => {
  const msg = ev.data as InitMsg | { type: 'stop' };
  if (msg.type === 'stop') { running = false; return; }
  if (msg.type !== 'init') return;

  workerId = msg.workerId;
  world = SharedWorld.attach(msg.memory);
  const core = await instantiateCore(world, msg.wasmUrl);
  ring = new TaskRing(world.control, world.sab);
  channel = new MeshChannel(world.control);

  running = true;
  (self as unknown as Worker).postMessage({ type: 'ready', workerId });

  // ── Laço principal: consome tarefas até a fila esvaziar ────────────────
  let idleSpins = 0;
  while (running) {
    const task = ring.claim();
    if (!task) {
      idleSpins++;
      // Sem trabalho: bloqueia de verdade (Atomics.wait) — CPU em repouso.
      const head = Atomics.load(world.control, 0);
      Atomics.wait(world.control, 0, head, 40);
      if (idleSpins > 400 && ring.pending() === 0) idleSpins = 0;
      continue;
    }
    idleSpins = 0;
    const res = runChunkJob(core, world, task, workerId, channel!);
    ring.markDone();
    // Diagnóstico opcional (apenas quando o debug está ligado no worker).
    if ((self as unknown as { __VX_VERBOSE?: boolean }).__VX_VERBOSE) {
      (self as unknown as Worker).postMessage({
        type: 'job', workerId, slot: res.slot, verts: res.verts,
        quads: res.quads, genMs: res.genMs, meshMs: res.meshMs,
      });
    }
  }
};

export { MAX_WORKERS };
