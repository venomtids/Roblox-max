/**
 * ============================================================================
 *  POOL DE WORKERS DE CHUNK (lado da main thread)
 * ============================================================================
 *  Responsabilidades:
 *   1) Criar N workers (N = núcleos físicos - 1, limitado a 7) e entregar a
 *      ELES a WebAssembly.Memory compartilhada por structured clone.
 *   2) Publicar as tarefas dos 576 chunks em ordem espiral (do spawn para fora).
 *   3) Consumir os meshes prontos lendo DIRETO da memória — sem postMessage de
 *      geometria, sem cópia, sem serialização. Custo por chunk: ~1 µs + upload.
 */

import { SharedWorld, MAX_WORKERS, chunkIndex } from '../core/memory';
import { MeshChannel, TaskRing, JOB_FULL, JOB_REMESH } from '../world/tasks';
import { ST_QUEUED, ST_READY, W_CHUNKS_X, W_CHUNKS_Y, W_CHUNKS_Z, LAYOUT } from '../core/memory';

export interface PoolOptions {
  /** Nº máximo de workers (default: núcleos - 1, teto 7). */
  maxWorkers?: number;
  wasmUrl?: string;
}

export class ChunkPool {
  readonly workers: Worker[] = [];
  readonly channel: MeshChannel;
  private ring: TaskRing;
  readonly workerCount: number;
  readyCount = 0;
  /** Estatísticas agregadas (para o HUD). */
  stats = { jobs: 0, meshMs: 0, genMs: 0, verts: 0 };
  private onProgress?: (done: number, total: number) => void;

  constructor(private readonly world: SharedWorld, opts: PoolOptions = {}) {
    const hw = (navigator.hardwareConcurrency || 4) - 1;
    this.workerCount = Math.max(1, Math.min(opts.maxWorkers ?? hw, MAX_WORKERS));
    this.channel = new MeshChannel(world.control);
    this.ring = new TaskRing(world.control, world.sab);
    this.wasmUrl = opts.wasmUrl ?? '/wasm/core.wasm';
  }
  private wasmUrl: string;

  /**
   * Cria os workers e espera todos ficarem prontos.
   *
   * Dois pontos que podem falhar no mundo real e precisam de fallback:
   *  1) `postMessage({ memory })` — o structured clone de `WebAssembly.Memory`
   *     é o que dá aos workers o MESMO linear memory. Em navegadores sem esse
   *     suporte o clone lança `DataCloneError`.
   *  2) worker que nunca responde (CSP, erro de módulo, memória).
   * Nos dois casos lançamos — e a engine volta para o modo single-thread.
   *
   * @throws Error quando threads não são viáveis neste navegador
   */
  async spawn(): Promise<number> {
    const t0 = performance.now();
    const jobs: Promise<void>[] = [];

    const timeout = (ms: number) => new Promise<never>((_, reject) =>
      setTimeout(() => reject(new Error('worker não respondeu a tempo')), ms));

    try {
      for (let i = 0; i < this.workerCount; i++) {
        // As opções precisam ser LITERAIS para o Vite pré-bundlar o worker.
        const w = new Worker(new URL('../workers/chunkWorker.ts', import.meta.url), { type: 'module' });
        this.workers.push(w);
        jobs.push(new Promise<void>((resolve, reject) => {
          const onMsg = (ev: MessageEvent) => {
            if (ev.data?.type === 'ready') { w.removeEventListener('message', onMsg); resolve(); }
            else if (ev.data?.type === 'error') { reject(new Error(String(ev.data.message))); }
          };
          w.addEventListener('message', onMsg);
          w.addEventListener('error', () => reject(new Error('falha ao carregar o worker de chunk')));
        }));
      }
      // A Memory é clonável via structured clone: é o que permite os workers
      // importarem o MESMO linear memory sem nenhuma cópia.
      for (let i = 0; i < this.workerCount; i++) {
        this.workers[i].postMessage({ type: 'init', memory: this.world.memory, wasmUrl: this.wasmUrl, workerId: i });
      }
      await Promise.race([Promise.all(jobs), timeout(6000)]);
    } catch (err) {
      this.shutdown();
      throw err;
    }
    return performance.now() - t0;
  }

  /** Publica a geração+mesh de todos os chunks, em espiral a partir do centro. */
  scheduleAll(centerCx: number, centerCz: number, onProgress?: (done: number, total: number) => void): void {
    this.onProgress = onProgress;
    const order: number[] = [];
    for (let cy = 0; cy < W_CHUNKS_Y; cy++) {
      for (let cz = 0; cz < W_CHUNKS_Z; cz++) {
        for (let cx = 0; cx < W_CHUNKS_X; cx++) order.push(chunkIndex(cx, cy, cz));
      }
    }
    // Ordena por distância ao spawn (materializa o mundo do jogador para fora)
    order.sort((a, b) => {
      const ax = a % W_CHUNKS_X - centerCx, az = Math.floor(a / W_CHUNKS_X) % W_CHUNKS_Z - centerCz;
      const bx = b % W_CHUNKS_X - centerCx, bz = Math.floor(b / W_CHUNKS_X) % W_CHUNKS_Z - centerCz;
      return (ax * ax + az * az) - (bx * bx + bz * bz);
    });
    for (const slot of order) {
      if (this.world.state[slot] !== 0) continue;         // ST_FREE
      Atomics.store(this.world.state, slot, ST_QUEUED);
      const cx = slot % W_CHUNKS_X;
      const cz = Math.floor(slot / W_CHUNKS_X) % W_CHUNKS_Z;
      const cy = Math.floor(slot / (W_CHUNKS_X * W_CHUNKS_Z));
      this.ring.push(JOB_FULL, cx, cy, cz);
    }
  }

  /** Marca um chunk (e vizinhos de borda) como sujo e pede remesh. */
  remesh(slot: number, alsoNeighbors = false): void {
    const cx = slot % W_CHUNKS_X;
    const cz = Math.floor(slot / W_CHUNKS_X) % W_CHUNKS_Z;
    const cy = Math.floor(slot / (W_CHUNKS_X * W_CHUNKS_Z));
    Atomics.store(this.world.state, slot, ST_QUEUED);
    this.ring.push(JOB_REMESH, cx, cy, cz);
    if (alsoNeighbors) {
      for (const [dx, dz] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
        const nx = cx + dx, nz = cz + dz;
        if (nx < 0 || nz < 0 || nx >= W_CHUNKS_X || nz >= W_CHUNKS_Z) continue;
        const ns = chunkIndex(nx, cy, nz);
        Atomics.store(this.world.state, ns, ST_QUEUED);
        this.ring.push(JOB_REMESH, nx, cy, nz);
      }
    }
  }

  /**
   * Consome meshes prontos. Chamada uma vez por frame: custo ~0 quando nada
   * ficou pronto (apenas N leituras atômicas).
   * @returns nº de chunks entregues ao renderer neste frame
   */
  drain(upload: (worker: number, slot: number) => void): number {
    let delivered = 0;
    for (let w = 0; w < this.workerCount; w++) {
      if (this.channel.pending(w) <= 0) continue;
      const slot = new Uint32Array(this.world.sab, this.world.meshPtr(w) + 40, 1)[0];
      upload(w, slot);
      this.channel.consumerAck(w);
      delivered++;
      this.stats.jobs++;
      this.readyCount++;
    }
    if (delivered && this.onProgress) {
      this.onProgress(this.ring.totalDone(), this.ring.totalPublished());
    }
    return delivered;
  }

  progress(): { done: number; total: number } {
    return { done: this.ring.totalDone(), total: this.ring.totalPublished() };
  }

  shutdown(): void {
    for (const w of this.workers) { w.postMessage({ type: 'stop' }); w.terminate(); }
    this.workers.length = 0;
  }
}
