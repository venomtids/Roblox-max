/// <reference lib="webworker" />
/**
 * ============================================================================
 *  WORKER DE FÍSICA — simulação em lote fora da main thread
 * ============================================================================
 *  Protocolo (tudo Atomics, zero postMessage por frame):
 *    main  → Atomics.add(C_PHYS_TICK, 1) + notify     "simule o passo"
 *    worker→ lê dt de C_PHYS_DT (f32) e roda simulateBodies()
 *    worker→ Atomics.add(C_PHYS_DONE, 1) + notify     "posições prontas"
 *
 *  A main thread NUNCA espera: ela agenda um passo à frente e segue
 *  renderizando com as posições do último passo concluído. É isso que
 *  mantém 60 FPS mesmo com a física saturada (pipeline assíncrono).
 */

import { SharedWorld, C_PHYS_TICK, C_PHYS_DONE, C_PHYS_DT, C_AWAKE, C_EDIT_EPOCH, C_EDIT_X, C_EDIT_Y, C_EDIT_Z, LAYOUT, PHYS_SCALE } from '../core/memory';
import { instantiateCore } from '../core/wasm';

interface InitMsg {
  type: 'init';
  memory: WebAssembly.Memory;
  wasmUrl: string;
  bodyCount: number;
}

let world: SharedWorld | null = null;
let running = false;

self.onmessage = async (ev: MessageEvent) => {
  const msg = ev.data as InitMsg | { type: 'stop' };
  if (msg.type === 'stop') { running = false; Atomics.notify(world!.control, C_PHYS_TICK); return; }
  if (msg.type !== 'init') return;

  world = SharedWorld.attach(msg.memory);
  const core = await instantiateCore(world, msg.wasmUrl);
  const control = world.control;
  const bodyCount = msg.bodyCount;
  const scratch = world.arenaPtr(7);          // arena dedicada à física
  const dtView = new Float32Array(world.sab, LAYOUT.control + C_PHYS_DT * 4, 1);

  let lastTick = Atomics.load(control, C_PHYS_TICK);
  let lastEdit = Atomics.load(control, C_EDIT_EPOCH);

  running = true;
  (self as unknown as Worker).postMessage({ type: 'ready', workerId: 'physics' });

  while (running) {
    Atomics.wait(control, C_PHYS_TICK, lastTick, 200);
    const tick = Atomics.load(control, C_PHYS_TICK);
    if (tick === lastTick) continue;
    lastTick = tick;

    // Edição do mundo → acorda corpos próximos (efeito "explosão" ao cavar).
    const edit = Atomics.load(control, C_EDIT_EPOCH);
    if (edit !== lastEdit) {
      lastEdit = edit;
      core.wakeBodies(LAYOUT.bodies, bodyCount,
        Atomics.load(control, C_EDIT_X) / PHYS_SCALE,
        Atomics.load(control, C_EDIT_Y) / PHYS_SCALE,
        Atomics.load(control, C_EDIT_Z) / PHYS_SCALE, 6.0);
    }

    const dt = dtView[0] || 1 / 60;
    const awake = core.simulateBodies(world.headerPtr, LAYOUT.bodies, bodyCount, dt, scratch);
    Atomics.store(control, C_AWAKE, awake);
    Atomics.add(control, C_PHYS_DONE, 1);
    Atomics.notify(control, C_PHYS_DONE);
  }
};
