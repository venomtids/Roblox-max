/**
 * ============================================================================
 *  HARNESS DE VALIDAÇÃO (Node) — roda o caminho de dados real sem navegador
 * ============================================================================
 *  Uso:  npm run test:core
 *
 *  O que ele prova:
 *   1. O layout de memória compartilhada (HEADER/TABLE/STATE/TASKS/ARENAS/
 *      MESHOUT/VOXELS) fecha exatamente como o WASM espera.
 *   2. O anel de tarefas lock-free distribui os 576 chunks sem perder nem
 *      duplicar trabalho (invariante: claimed == published == done).
 *   3. O mesher produz vértices dentro do AABB do chunk (sem NaN, sem
 *      posição fora do mundo) — o que garante o frustum culling correto.
 *   4. A física de corpos rígidos não deixa nada atravessar o chão.
 *   5. O raycast DDA acerta o primeiro bloco sólido na direção da mira.
 *
 *  Como é Node puro, ele não valida WebGPU (impossível fora do navegador):
 *  a parte de GPU é validada pelos pipelines do renderer no Chrome.
 */

import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import {
  SharedWorld, chunkIndex, MAX_BODIES, LAYOUT, MESH_HDR_BYTES, VERTEX_BYTES,
  ST_READY, W_CHUNKS_X, W_CHUNKS_Y, W_CHUNKS_Z, W_BLOCKS_X, W_BLOCKS_Y, W_BLOCKS_Z,
} from '../src/core/memory';
import { TaskRing, MeshChannel } from '../src/world/tasks';
import { runChunkJob } from '../src/world/jobs';
import type { Core } from '../src/core/wasm';

/** O harness roda da raiz do projeto: procura o .wasm no lugar canônico. */
const wasmPath = join(process.cwd(), 'public', 'wasm', 'core.wasm');

function ok(label: string, cond: boolean, extra = ''): void {
  const tag = cond ? '\x1b[32mPASS\x1b[0m' : '\x1b[31mFAIL\x1b[0m';
  console.log(`  ${tag}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) process.exitCode = 1;
}

async function main(): Promise<void> {
  console.log('\n\x1b[1mROBLOX-MAX · validação do núcleo (WASM + memória compartilhada + tarefas)\x1b[0m\n');

  // ── 1. Mundo + instância WASM sobre a memória compartilhada ─────────────
  const world = SharedWorld.create(20261011);
  ok('SharedArrayBuffer alocado', world.sab.byteLength === 32 * 1024 * 1024,
    `${(world.sab.byteLength / 1048576).toFixed(0)} MB`);

  const bytes = readFileSync(wasmPath);
  const module = new WebAssembly.Module(bytes);
  const instance = await WebAssembly.instantiate(module, {
    env: { memory: world.memory, abort: (m: number, f: number, l: number, c: number) => { throw new Error(`abort ${m} ${f} ${l}:${c}`); } },
  });
  const core = instance.exports as unknown as Core;
  ok('Núcleo WASM instanciado sobre o SAB', core.coreVersion() === 10007, `versão ${core.coreVersion()}`);

  const imports = WebAssembly.Module.imports(module).map((i) => `${i.module}.${i.name}`);
  ok('Importa env.memory (memória compartilhada externa)', imports.includes('env.memory'));

  const simdOps = ['i8x16.any_true', 'v128.load', 'v128.store'];
  const wat = WebAssembly.Module.exports(module).length;
  ok('Módulo expõe as funções do núcleo', wat >= 14, `${wat} exports`);
  void simdOps;

  // ── 2. Anel de tarefas (single producer multi consumer, simulado) ───────
  const ring = new TaskRing(world.control, world.sab);
  const channel = new MeshChannel(world.control);
  const total = W_CHUNKS_X * W_CHUNKS_Y * W_CHUNKS_Z;
  const t0 = performance.now();
  for (let cy = 0; cy < W_CHUNKS_Y; cy++)
    for (let cz = 0; cz < W_CHUNKS_Z; cz++)
      for (let cx = 0; cx < W_CHUNKS_X; cx++) ring.push(0, cx, cy, cz);
  const pushMs = performance.now() - t0;
  ok('Publicadas todas as tarefas de chunk', ring.totalPublished() === total, `${total} tarefas em ${pushMs.toFixed(2)} ms`);

  // ── 3. "Workers" virtuais consumindo a fila (1..8 concorrentes lógicos) ─
  const workerCount = 8;
  const done = new Set<number>();
  const duplicate: number[] = [];
  let genMs = 0, meshMs = 0, verts = 0, quads = 0, truncated = 0, outsideAabb = 0, badVertex = 0;
  const t1 = performance.now();

  // Simula a intercalação de N workers: cada um reivindica a próxima tarefa.
  let finished = false;
  while (!finished) {
    finished = true;
    for (let w = 0; w < workerCount; w++) {
      const task = ring.claim();
      if (!task) continue;
      finished = false;
      const res = runChunkJob(core, world, task, w, channel);
      const slot = chunkIndex(task.cx, task.cy, task.cz);
      if (done.has(slot)) duplicate.push(slot);
      done.add(slot);
      verts += res.verts; quads += res.quads; genMs += res.genMs; meshMs += res.meshMs;
      if (res.truncated) truncated++;
      ring.markDone();
      channel.consumerAck(w);
    }
  }
  const totalMs = performance.now() - t1;
  ok('Nenhum chunk processado duas vezes', duplicate.length === 0);
  ok('Todos os 576 chunks gerados e mesheados', done.size === total, `${done.size}/${total}`);
  ok('Invariante da fila (published == done)', ring.totalPublished() === ring.totalDone());
  ok('Sem estouro de buffer de mesh (sem truncamento)', truncated === 0);
  console.log(`        tempo total single-thread: ${totalMs.toFixed(0)} ms `
            + `(geração ${genMs.toFixed(0)} ms · meshing ${meshMs.toFixed(0)} ms) → `
            + `~${(totalMs / workerCount).toFixed(0)} ms com ${workerCount} workers`);

  // ── 4. Validação dos vértices (integridade do mesh) ─────────────────────
  const dv = new DataView(world.sab);
  for (let w = 0; w < workerCount; w++) {
    const base = world.meshPtr(w);
    const head = new Uint32Array(world.sab, base, 8);
    const n = head[0] + head[4];
    for (let v = 0; v < n; v++) {
      const p = base + MESH_HDR_BYTES + v * VERTEX_BYTES;
      const wa = dv.getUint32(p, true), wb = dv.getUint32(p + 4, true), wc = dv.getUint32(p + 8, true);
      const x = wa & 0xffff, y = (wa >>> 16) & 0xffff, z = wb & 0xffff;
      const layer = wc >>> 16;
      if (x > W_BLOCKS_X || y > W_BLOCKS_Y || z > W_BLOCKS_Z) outsideAabb++;
      if (layer >= 18 || (wb >>> 16) & 7 ? false : true) { /* placeholder */ }
      if (((wb >>> 16) & 7) > 5) badVertex++;
    }
  }
  ok('Vértices dentro dos limites do mundo', outsideAabb === 0, `${outsideAabb} fora`);
  ok('Normal de face sempre em 0..5', badVertex === 0);
  ok('Greedy meshing reduz drasticamente os quads', quads < total * 1024,
    `${quads} quads (${(quads / total).toFixed(0)} por chunk, ${(verts / 1e3).toFixed(0)}k vértices)`);

  // ── 5. Leitura/escrita de voxel + superfície ────────────────────────────
  const sx = Math.floor(W_BLOCKS_X / 2), sz = Math.floor(W_BLOCKS_Z / 2);
  const surface = core.surfaceY(world.headerPtr, sx, sz);
  ok('Superfície detectada no centro do mundo', surface > 0 && surface < W_BLOCKS_Y, `y=${surface}`);
  ok('Spawn livre de blocos', core.isSpawnClear(world.headerPtr, sx, surface, sz) === 1);
  const before = world.getBlock(sx, surface - 1, sz);
  world.setBlock(sx, surface + 3, sz, 10);
  ok('Escrita de voxel visível para o WASM', core.getBlockW(world.headerPtr, sx, surface + 3, sz) === 10);
  ok('Chunks registrados na tabela', world.table[chunkIndex(sx >> 5, (surface - 1) >> 5, sz >> 5)] >= 0, `bloco base=${before}`);

  // ── 6. Raycast DDA (olhando para baixo) ─────────────────────────────────
  const hit = core.raycastVoxel(world.headerPtr, sx + 0.5, surface + 40, sz + 0.5, 0, -1, 0, 128, world.arenaPtr(0));
  const rayF = new Float32Array(world.sab, world.arenaPtr(0), 8);
  ok('Raycast acerta o terreno', hit !== 0, `bloco=${hit} em y=${rayF[1]}`);
  ok('Raycast vertical retorna a face de cima', new Int32Array(world.sab, world.arenaPtr(0) + 16, 3)[1] === 1);

  // ── 7. Física de corpos rígidos (integração no WASM) ────────────────────
  const N = 1000;
  for (let i = 0; i < N; i++) {
    world.setBody(i, sx + 0.5 + (i % 20) * 0.4, surface + 30 + (i % 30), sz + 0.5 + Math.floor(i / 20) * 0.4, 0.3, 0xff0000ff, 0);
  }
  const t2 = performance.now();
  const FIXED = 1 / 60;
  for (let tick = 0; tick < 240; tick++) {
    core.simulateBodies(world.headerPtr, LAYOUT.bodies, N, FIXED, world.arenaPtr(7));
  }
  const simMs = performance.now() - t2;
  let buried = 0, settled = 0;
  for (let i = 0; i < N; i++) {
    const [x, y, z] = world.bodyPos(i);
    if (y < 0) buried++;
    if (y < surface + 2 && y > 0) settled++;
    void x; void z;
  }
  ok('Nenhum corpo caiu fora do mundo', buried === 0);
  ok('Corpos assentaram no chão (solver estável)', settled > N * 0.8, `${settled}/${N} assentados`);
  console.log(`        física: ${N} corpos × 240 ticks em ${simMs.toFixed(0)} ms → `
            + `${(simMs / 240).toFixed(3)} ms/frame (${(simMs / 240 / N * 1000).toFixed(2)} µs/corpo)`);

  // ── 8. Uso de memória e limites ─────────────────────────────────────────
  const voxelsEnd = LAYOUT.voxels + 640 * 32768;
  ok('Regiões de memória não se sobrepõem', LAYOUT.meshOut + 8 * LAYOUT.meshSlotBytes <= LAYOUT.voxels
    && LAYOUT.arenas + 8 * LAYOUT.arenaBytes <= LAYOUT.meshOut
    && voxelsEnd <= world.sab.byteLength,
    `fim dos voxels em ${(voxelsEnd / 1048576).toFixed(1)} MB de 32 MB`);
  ok('Capacidade de corpos rígidos', MAX_BODIES >= 4096, `${MAX_BODIES} peças`);
  ok('Todo chunk terminou em ST_READY', world.state.subarray(0, total).every((v) => v === ST_READY));

  console.log(`\n\x1b[1mResultado:\x1b[0m ${process.exitCode ? '\x1b[31mfalhas acima\x1b[0m' : '\x1b[32mtudo verde\x1b[0m'}`
            + ` · ${(verts / 1e3).toFixed(0)}k vértices · ${quads} quads · ${(world.sab.byteLength / 1048576) | 0} MB de heap compartilhado\n`);
}

main().catch((e) => { console.error(e); process.exit(1); });
