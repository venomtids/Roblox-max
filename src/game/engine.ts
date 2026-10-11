/**
 * ============================================================================
 *  ENGINE — orquestrador do ROBLOX-MAX
 * ============================================================================
 *  Fluxo de um frame (orçamento alvo: < 3 ms de CPU na main thread):
 *
 *    [entrada] → [física do jogador no WASM] → [tick de física dos corpos
 *    (worker)] → [drenar meshes prontos (workers) → upload GPU] →
 *    [instâncias dos corpos (SAB → GPU)] → [render WebGPU] → [HUD 5 Hz]
 *
 *  Nenhuma dessas etapas aloca memória por frame: tudo é buffer reutilizado.
 *  A main thread é "burra de propósito": ela não gera, não mesha e não
 *  simula corpos — só decide, publica tarefas e desenha.
 */

import {
  SharedWorld, MAX_BODIES, W_BLOCKS_X, W_BLOCKS_Z, SEA_LEVEL,
  C_PHYS_TICK, C_PHYS_DONE, C_PHYS_DT, C_EDIT_EPOCH, C_EDIT_X, C_EDIT_Y, C_EDIT_Z,
  chunkIndex, LAYOUT, W_CHUNKS_X, W_CHUNKS_Y, W_CHUNKS_Z, PHYS_SCALE,
} from '../core/memory';
import { instantiateCore, WASM_SHARED, WASM_PLAIN, type Core } from '../core/wasm';
import { ChunkPool } from '../world/chunkPool';
import { buildAtlas } from '../render/atlas';
import { Renderer, type CameraState, type ChunkBuffer } from '../render/renderer';
import { Player, PLAYER_HALF, PLAYER_HEIGHT, type PlayerInput } from './player';
import { Input } from './input';
import { clamp } from '../core/math';

export interface HotbarItem { id: number; name: string; color: string; key: string }

export const HOTBAR: HotbarItem[] = [
  { id: 1, name: 'Grama', color: '#5fbf3f', key: '1' },
  { id: 3, name: 'Pedra', color: '#8f8f96', key: '2' },
  { id: 2, name: 'Terra', color: '#8a5f3a', key: '3' },
  { id: 4, name: 'Areia', color: '#e0cf95', key: '4' },
  { id: 9, name: 'Madeira', color: '#c08b4e', key: '5' },
  { id: 10, name: 'Tijolo', color: '#a04a3c', key: '6' },
  { id: 11, name: 'Vidro', color: '#bfe6f5', key: '7' },
  { id: 15, name: 'Lâmpada', color: '#ffd97a', key: '8' },
  { id: 6, name: 'Tronco', color: '#7b5a34', key: '9' },
];

export interface EngineSettings {
  renderScale: number;      // 0.5 – 2.0 (multiplicador sobre devicePixelRatio)
  fov: number;              // graus
  viewDistance: number;     // em blocos
  daySpeed: number;         // fração do dia por segundo (0 = travado)
  sensitivity: number;
  bodyCount: number;        // corpos rígidos ativos
  wireframe: boolean;
}

export const DEFAULT_SETTINGS: EngineSettings = {
  renderScale: Math.min(1, (globalThis.devicePixelRatio || 1) > 1.5 ? 0.85 : 1),
  fov: 78,
  viewDistance: 320,
  daySpeed: 1 / 240,
  sensitivity: 1,
  bodyCount: 900,
  wireframe: false,
};

export interface EngineHooks {
  stage(label: string, state: 'ok' | 'warn' | 'fail' | 'run', detail?: string): void;
  progress(frac: number, label: string): void;
  ready(stats: { workers: number; buildMs: number; adapter: string }): void;
  fatal(message: string, help?: string): void;
}

export interface HudInfo {
  fps: number; frameMs: number; cpuMs: number; gpuMs: number;
  drawCalls: number; triangles: number; chunksDrawn: number; chunksCulled: number;
  chunksReady: number; chunksTotal: number;
  instances: number; physicsMs: number; workers: number;
  x: number; y: number; z: number; speed: number;
  biome: string; clock: string;
  memMB: number; flying: boolean; onGround: boolean; inWater: boolean;
  selected: string; edits: number; verts: number;
  /** Há um bloco sob a mira (used pelo crosshair)? */
  target: boolean;
}

const FIXED_DT = 1 / 60;

export class Engine {
  world!: SharedWorld;
  core!: Core;
  renderer!: Renderer;
  pool?: ChunkPool;
  player!: Player;
  input!: Input;
  settings: EngineSettings = { ...DEFAULT_SETTINGS };
  adapterName = 'GPU';
  workers = 0;
  buildMs = 0;
  timeSec = 0;
  timeOfDay = 0.32;                 // começa de manhã
  selected = 0;
  edits = 0;
  private physWorker: Worker | null = null;
  private scratch = 0;
  private bodyScratch = 0;
  private inputState: PlayerInput = { forward: 0, back: 0, left: 0, right: 0, jump: false, run: false, crouch: false, fly: false, mouseDX: 0, mouseDY: 0 };
  private cam: CameraState = { position: new Float32Array(3), yaw: 0, pitch: 0, fovDeg: 78, frustum: new Float32Array(24), maxDist: 320 };
  private eye = new Float32Array(3);
  private look = new Float32Array(3);
  private rayF32!: Float32Array;
  private rayI32!: Int32Array;
  private dtView!: Float32Array;
  private loadCam!: CameraState;
  private hitBlock = new Int32Array(3);
  private hitNormal = new Int32Array(3);
  private hasHit = false;
  private fogColor: [number, number, number] = [0.62, 0.75, 0.92];
  private sunDir: [number, number, number] = [0.4, 0.7, 0.3];
  private ambient = 0.8;
  private dayFactor = 1;
  private acc = 0; private physAcc = 0;
  private frames = 0; private fpsTimer = 0; private fps = 0;
  private hudTimer = 0;
  private cpuMs = 0; private physMs = 0;
  private totalVerts = 0;
  private running = false;
  private raf = 0;
  private lastNow = 0;
  private hooks: EngineHooks;
  /** Build do núcleo em uso (compartilhado ou single-thread). */
  wasmUrl = WASM_SHARED;
  /** Handler do HUD (definido após a criação da interface, que depende da engine). */
  onHud: ((info: HudInfo) => void) | null = null;

  private constructor(hooks: EngineHooks) { this.hooks = hooks; }

  // ═════════════════════════ BOOT ═════════════════════════════════════════
  static async create(canvas: HTMLCanvasElement, hooks: EngineHooks, seed = 1337): Promise<Engine> {
    const eng = new Engine(hooks);
    const t0 = performance.now();

    // ── 1. WebGPU ───────────────────────────────────────────────────────
    hooks.stage('WebGPU', 'run', 'solicitando adaptador de alto desempenho');
    let device: GPUDevice; let adapterInfo = 'GPU';
    try {
      const { device: dev, adapter } = await Renderer.createDevice();
      device = dev;
      const info = (adapter as unknown as { info?: { vendor?: string; architecture?: string } }).info;
      adapterInfo = info ? `${info.vendor ?? ''} ${info.architecture ?? ''}`.trim() || 'WebGPU' : 'WebGPU';
      hooks.stage('WebGPU', 'ok', adapterInfo);
    } catch (err) {
      hooks.stage('WebGPU', 'fail', (err as Error).message);
      throw err;
    }

    // ── 2. Memória compartilhada + núcleo WASM ──────────────────────────
    // Threads exigem SAB + isolamento de origem; sem isso o jogo roda igual,
    // apenas com geração/meshing na própria main thread (fatiado por frame).
    const crossOrigin = (globalThis as unknown as { crossOriginIsolated?: boolean }).crossOriginIsolated === true;
    hooks.stage('Memória compartilhada', crossOrigin ? 'run' : 'warn',
      crossOrigin ? 'SharedArrayBuffer ativo' : 'SAB indisponível — modo single-thread');

    eng.world = SharedWorld.create(seed);
    eng.core = await instantiateCore(eng.world);
    eng.wasmUrl = eng.world.isShared ? WASM_SHARED : WASM_PLAIN;
    const simd = detectSimd();
    hooks.stage('WASM SIMD', simd ? 'ok' : 'warn', simd ? 'v128/i8x16 ativos' : 'sem SIMD — fallback escalar');

    // ── 3. Workers (com fallback gracioso) ──────────────────────────────
    const threads = typeof Worker !== 'undefined' && crossOrigin;
    if (threads) {
      try {
        eng.pool = new ChunkPool(eng.world, {
          maxWorkers: Math.min(7, Math.max(1, (navigator.hardwareConcurrency || 4) - 1)),
          wasmUrl: eng.wasmUrl,
        });
        eng.workers = await eng.pool.spawn();
        hooks.stage('Multithreading', 'ok', `${eng.workers} workers de chunk + 1 de física`);
      } catch (err) {
        eng.pool = undefined;
        eng.workers = 0;
        hooks.stage('Multithreading', 'warn', `threads indisponíveis (${(err as Error).message}) — modo single-thread`);
      }
    } else {
      eng.workers = 0;
      hooks.stage('Multithreading', 'warn', 'rodando na main thread (sem isolamento de origem)');
    }

    // ── 4. GPU: pipelines + atlas procedural ────────────────────────────
    const atlasPixels = buildAtlas();
    eng.renderer = new Renderer(canvas, device, atlasPixels, 4);
    hooks.stage('Pipeline gráfico', 'ok', 'atlas procedural 18 camadas · MSAA 4×');

    eng.input = new Input(canvas);
    eng.scratch = LAYOUT.arenas + 8 * LAYOUT.arenaBytes - 512;   // 512 B no fim da última arena
    eng.bodyScratch = eng.scratch + 64;
    eng.player = new Player(eng.world, eng.core, eng.scratch + 128);
    eng.rayF32 = new Float32Array(eng.world.sab, eng.scratch, 4);
    eng.rayI32 = new Int32Array(eng.world.sab, eng.scratch, 8);
    eng.dtView = new Float32Array(eng.world.sab, LAYOUT.control + C_PHYS_DT * 4, 1);
    eng.loadCam = {
      position: new Float32Array(3), yaw: 0, pitch: -0.42,
      fovDeg: 70, frustum: new Float32Array(24), maxDist: 400,
    };
    eng.bodies = new BodySystem(eng);

    eng.buildMs = performance.now() - t0;
    return eng;
  }

  // ═════════════════════ CARREGAMENTO DO MUNDO ════════════════════════════
  async loadWorld(): Promise<void> {
    const total = W_CHUNKS_X * W_CHUNKS_Y * W_CHUNKS_Z;
    // Centro do mundo = ponto de spawn
    const cx = Math.floor(W_CHUNKS_X / 2), cz = Math.floor(W_CHUNKS_Z / 2);

    if (this.pool) {
      this.pool.scheduleAll(cx, cz);
    } else {
      // Fallback single-thread: gera e mesha na main thread, em fatias por frame.
      this.scheduleMainThreadFallback(cx, cz);
    }

    await new Promise<void>((resolve) => {
      let firstFrame = true;
      const pump = () => {
        this.pumpChunks();
        const p = this.chunkProgress();
        this.hooks.progress(total ? p.done / total : 1, `Gerando e otimizando o mundo · ${p.done}/${total} chunks`);
        // Pré-visualização: o mundo materializa atrás da tela de carregamento.
        if (p.done > 0) {
          try { this.previewRender(p.done >= total); } catch { /* nunca falhar o boot por isso */ }
          firstFrame = false;
        }
        if (p.done >= total) { resolve(); return; }
        requestAnimationFrame(pump);
      };
      pump();
      void firstFrame;
    });

    // Posiciona o jogador na superfície do spawn
    const spawnX = Math.floor(W_BLOCKS_X / 2), spawnZ = Math.floor(W_BLOCKS_Z / 2);
    this.player.spawn(spawnX, spawnZ);
    this.spawnStarterBodies();
    this.hooks.progress(1, 'Pronto');
  }

  /** Frame de pré-visualização durante o carregamento (câmera panorâmica). */
  private previewRender(done: boolean): void {
    const spawnX = W_BLOCKS_X / 2, spawnZ = W_BLOCKS_Z / 2;
    const y = this.core.surfaceY(this.world.headerPtr, Math.floor(spawnX), Math.floor(spawnZ));
    const cam = this.loadCam;
    cam.position[0] = spawnX + (done ? 0 : 40);
    cam.position[1] = y + 22;
    cam.position[2] = spawnZ + 46;
    cam.yaw = done ? 0 : -0.6;
    this.renderer.resize(innerWidth, innerHeight, this.settings.renderScale, globalThis.devicePixelRatio || 1);
    this.renderer.updateCamera(cam, this.timeSec, this.fogColor, 0.0028, this.sunDir, this.ambient, this.dayFactor);
    this.renderer.updateBodies(this.world.sab, 0);
    this.renderer.render(cam, this.renderer.drawables);
  }

  private scheduleMainThreadFallback(cx: number, cz: number): void {
    const order: number[] = [];
    for (let i = 0; i < W_CHUNKS_X * W_CHUNKS_Y * W_CHUNKS_Z; i++) order.push(i);
    order.sort((a, b) => {
      const ax = a % W_CHUNKS_X - cx, az = Math.floor(a / W_CHUNKS_X) % W_CHUNKS_Z - cz;
      const bx = b % W_CHUNKS_X - cx, bz = Math.floor(b / W_CHUNKS_X) % W_CHUNKS_Z - cz;
      return ax * ax + az * az - (bx * bx + bz * bz);
    });
    this.fallbackQueue = order;
  }
  private fallbackQueue: number[] = [];

  /** Drena meshes dos workers (ou processa N chunks na main thread). */
  private pumpChunks(): void {
    const pool = this.pool;
    if (pool) {
      pool.drain((worker, slot) => {
        this.renderer.uploadMesh(this.world.sab, worker, slot);
        this.totalVerts += this.readMeshVerts(worker);
      });
      return;
    }
    // Fallback: orçamento de 6 ms por frame para não travar a UI
    const t0 = performance.now();
    const worker = 0;
    while (this.fallbackQueue.length && performance.now() - t0 < 6) {
      const slot = this.fallbackQueue.shift()!;
      const cxc = slot % W_CHUNKS_X;
      const czc = Math.floor(slot / W_CHUNKS_X) % W_CHUNKS_Z;
      const cyc = Math.floor(slot / (W_CHUNKS_X * W_CHUNKS_Z));
      this.core.generateChunk(this.world.headerPtr, slot, cxc, cyc, czc);
      this.core.meshChunk(this.world.headerPtr, slot, cxc, cyc, czc, this.world.meshPtr(worker), LAYOUT.meshSlotBytes, this.world.arenaPtr(worker));
      this.world.table[slot] = slot;
      Atomics.store(this.world.state, slot, 2);
      this.renderer.uploadMesh(this.world.sab, worker, slot);
    }
  }

  private readMeshVerts(worker: number): number {
    const base = this.world.meshPtr(worker);
    const head = new Uint32Array(this.world.sab, base, 8);
    return head[0] + head[4];
  }

  private chunkProgress(): { done: number; total: number } {
    if (this.pool) return this.pool.progress();
    const total = W_CHUNKS_X * W_CHUNKS_Y * W_CHUNKS_Z;
    return { done: total - this.fallbackQueue.length, total };
  }

  // ═════════════════════ FÍSICA DOS CORPOS RÍGIDOS ════════════════════════
  bodies!: BodySystem;
  private physicsReady = false;

  private startPhysicsWorker(): void {
    if (!this.workers) return;
    const w = new Worker(new URL('../workers/physicsWorker.ts', import.meta.url), { type: 'module' });
    w.onmessage = (ev) => { if (ev.data?.type === 'ready') this.physicsReady = true; };
    w.postMessage({ type: 'init', memory: this.world.memory, wasmUrl: this.wasmUrl, bodyCount: MAX_BODIES });
    this.physWorker = w;
  }

  /** Distribui a cena inicial de peças (torre + pilha + arco). */
  private spawnStarterBodies(): void {
    const px = Math.round(this.player.pos[0]), pz = Math.round(this.player.pos[2]);
    let i = 0;
    const colors = [0xff4d6dff, 0x37c6ffff, 0x39ff88ff, 0xffb020ff, 0xb06bffff, 0xffe9a8ff];
    // Anel de peças pairando (caem e formam pilha — show de física no 1º segundo)
    for (let ring = 0; ring < 3 && i < this.settings.bodyCount; ring++) {
      const r = 7 + ring * 3;
      for (let a = 0; a < 20 && i < this.settings.bodyCount; a++, i++) {
        const ang = (a / 20) * Math.PI * 2 + ring;
        const x = px + Math.cos(ang) * r, z = pz + Math.sin(ang) * r;
        const y = this.core.surfaceY(this.world.headerPtr, Math.round(x), Math.round(z)) + 14 + ring * 2;
        const half = 0.35 + (i % 3) * 0.12;
        this.world.setBody(i, x + 0.5, y, z + 0.5, half, colors[i % colors.length], 0);
      }
    }
    // Peças dentro da "linha de arremesso" (uma torre de teste perto do player)
    this.startPhysicsWorker();
  }

  // ═══════════════════════ LOOP PRINCIPAL ═════════════════════════════════
  start(): void {
    if (this.running) return;
    this.running = true;
    this.lastNow = performance.now();
    const loop = (now: number) => {
      if (!this.running) return;
      this.frame(now);
      this.raf = requestAnimationFrame(loop);
    };
    this.raf = requestAnimationFrame(loop);
  }

  stop(): void { this.running = false; cancelAnimationFrame(this.raf); }

  private frame(now: number): void {
    const cpu0 = performance.now();
    const dtRaw = (now - this.lastNow) / 1000;
    this.lastNow = now;
    const dt = Math.min(0.1, Math.max(0.0001, dtRaw));
    this.timeSec += dt;

    // ── 1. Entrada + física do jogador (passo fixo = determinismo) ────────
    const input = this.input.snapshot(this.inputState);
    this.acc += dt;
    let steps = 0;
    while (this.acc >= FIXED_DT && steps < 5) {
      this.player.step(input, FIXED_DT);
      this.acc -= FIXED_DT; steps++;
    }
    if (steps === 0) { /* frames muito rápidos: mantém a última simulação */ }

    // ── 2. Tick de física dos corpos no worker (backpressure de 2 passos) ─
    const tPhys0 = performance.now();
    this.physAcc += dt;
    if (this.workers) {
      const pending = Atomics.load(this.world.control, C_PHYS_TICK) - Atomics.load(this.world.control, C_PHYS_DONE);
      let req = 0;
      while (this.physAcc >= FIXED_DT && req < 3 && pending + req < 3) { this.physAcc -= FIXED_DT; req++; }
      if (req > 0) {
        this.dtView[0] = FIXED_DT;
        Atomics.add(this.world.control, C_PHYS_TICK, req);
        Atomics.notify(this.world.control, C_PHYS_TICK);
      }
    } else {
      // Fallback: simula no WASM da main thread (custa ~0,4 ms/1000 corpos)
      while (this.physAcc >= FIXED_DT) {
        this.core.simulateBodies(this.world.headerPtr, LAYOUT.bodies, this.settings.bodyCount, FIXED_DT, this.bodyScratch);
        this.physAcc -= FIXED_DT;
      }
    }
    this.physMs = performance.now() - tPhys0;

    // ── 3. Drenar meshes prontos → VRAM ─────────────────────────────────
    this.pumpChunks();

    // ── 4. Céu / tempo / fog ────────────────────────────────────────────
    this.updateSky(dt);
    this.player.eyePosition(this.eye);
    this.player.lookDir(this.look);
    this.updateRaycast();
    this.handleEdits();
    this.bodies.update(dt);

    // ── 5. Render ───────────────────────────────────────────────────────
    const dpr = globalThis.devicePixelRatio || 1;
    this.renderer.resize(innerWidth, innerHeight, this.settings.renderScale, dpr);
    this.cam.position[0] = this.eye[0]; this.cam.position[1] = this.eye[1]; this.cam.position[2] = this.eye[2];
    this.cam.yaw = this.player.yaw; this.cam.pitch = this.player.pitch;
    this.cam.fovDeg = this.settings.fov;
    this.cam.maxDist = this.settings.viewDistance;
    const underWater = this.player.inWater && this.world.getBlock(
      Math.floor(this.eye[0]), Math.floor(this.eye[1]), Math.floor(this.eye[2])) === 5;
    const fog = underWater ? ([0.08, 0.35, 0.5] as [number, number, number]) : this.fogColor;
    const density = underWater ? 0.09 : 0.0032;
    this.renderer.updateCamera(this.cam, this.timeSec, fog, density, this.sunDir, this.ambient, this.dayFactor);
    this.renderer.setSelectionOutline(this.hitBlock[0], this.hitBlock[1], this.hitBlock[2], this.hasHit && this.input.locked);
    this.renderer.updateBodies(this.world.sab, this.settings.bodyCount);
    this.renderer.render(this.cam, this.renderer.drawables as ChunkBuffer[]);

    // ── 6. HUD (5 Hz) ───────────────────────────────────────────────────
    this.cpuMs = performance.now() - cpu0;
    this.frames++; this.fpsTimer += dt; this.hudTimer += dt;
    if (this.fpsTimer >= 0.5) { this.fps = this.frames / this.fpsTimer; this.frames = 0; this.fpsTimer = 0; }
    if (this.hudTimer >= 0.2) { this.hudTimer = 0; this.emitHud(); }
    this.input.endFrame();
  }

  private updateSky(dt: number): void {
    if (this.settings.daySpeed > 0) this.timeOfDay = (this.timeOfDay + dt * this.settings.daySpeed) % 1;
    const ang = (this.timeOfDay - 0.25) * Math.PI * 2;
    const elev = Math.sin(ang);
    const cosA = Math.cos(ang);
    this.sunDir[0] = cosA * 0.55; this.sunDir[1] = elev; this.sunDir[2] = 0.38;
    const l = Math.hypot(this.sunDir[0], this.sunDir[1], this.sunDir[2]) || 1;
    this.sunDir[0] /= l; this.sunDir[1] /= l; this.sunDir[2] /= l;

    this.dayFactor = clamp(elev * 2.4 + 0.28, 0, 1);
    this.ambient = 0.32 + 0.55 * this.dayFactor;
    // Cor do horizonte: quente no nascer/poente, azul de dia, escura à noite
    const sunset = clamp(1 - Math.abs(elev) * 4.5, 0, 1) * clamp(this.dayFactor * 3 + 0.35, 0, 1);
    const day = this.dayFactor;
    this.fogColor[0] = 0.06 + 0.48 * day + 0.42 * sunset;
    this.fogColor[1] = 0.08 + 0.58 * day + 0.22 * sunset;
    this.fogColor[2] = 0.14 + 0.72 * day - 0.10 * sunset;
  }

  private updateRaycast(): void {
    const id = this.core.raycastVoxel(
      this.world.headerPtr, this.eye[0], this.eye[1], this.eye[2],
      this.look[0], this.look[1], this.look[2], 8, this.scratch,
    );
    const f32 = this.rayF32;
    const i32 = this.rayI32;
    this.hasHit = id !== 0;
    if (this.hasHit) {
      this.hitBlock[0] = f32[0] | 0; this.hitBlock[1] = f32[1] | 0; this.hitBlock[2] = f32[2] | 0;
      this.hitNormal[0] = i32[4]; this.hitNormal[1] = i32[5]; this.hitNormal[2] = i32[6];
    }
  }

  private handleEdits(): void {
    if (!this.input.locked) return;
    const wheel = this.input.wheel;
    if (wheel) {
      this.selected = (this.selected + (wheel > 0 ? 1 : HOTBAR.length - 1)) % HOTBAR.length;
    }
    if (this.input.mouse0Pressed && this.hasHit) {
      const x = this.hitBlock[0], y = this.hitBlock[1], z = this.hitBlock[2];
      const id = this.world.getBlock(x, y, z);
      if (id !== 16) {   // bedrock é indestrutível
        if (this.world.setBlock(x, y, z, 0)) {
          this.markDirty(x, y, z);
          this.edits++;
          this.wakeNear(x, y, z, 4.5);
          this.bodies.puff(x + 0.5, y + 0.5, z + 0.5, id);
        }
      }
    }
    if (this.input.mouse2Pressed && this.hasHit) {
      const x = this.hitBlock[0] + this.hitNormal[0];
      const y = this.hitBlock[1] + this.hitNormal[1];
      const z = this.hitBlock[2] + this.hitNormal[2];
      if (!this.blockIntersectsPlayer(x, y, z)) {
        const item = HOTBAR[this.selected];
        if (this.world.setBlock(x, y, z, item.id)) {
          this.markDirty(x, y, z);
          this.edits++;
          this.wakeNear(x, y, z, 3.0);
        }
      }
    }
  }

  private blockIntersectsPlayer(x: number, y: number, z: number): boolean {
    const p = this.player.pos;
    const minY = p[1], maxY = p[1] + PLAYER_HEIGHT;
    return (x + 1 > p[0] - PLAYER_HALF && x < p[0] + PLAYER_HALF
         && y + 1 > minY && y < maxY
         && z + 1 > p[2] - PLAYER_HALF && z < p[2] + PLAYER_HALF);
  }

  /** Marca o chunk (e vizinhos de borda) para remesh. */
  private markDirty(x: number, y: number, z: number): void {
    const cx = x >> 5, cy = y >> 5, cz = z >> 5;
    const slot = chunkIndex(cx, cy, cz);
    const border = (x & 31) === 0 || (x & 31) === 31 || (z & 31) === 0 || (z & 31) === 31 || (y & 31) === 0 || (y & 31) === 31;
    if (this.pool) this.pool.remesh(slot, border);
    else {
      // Fallback: remesh imediato na main thread
      this.core.meshChunk(this.world.headerPtr, slot, cx, cy, cz, this.world.meshPtr(0), LAYOUT.meshSlotBytes, this.world.arenaPtr(0));
      this.renderer.uploadMesh(this.world.sab, 0, slot);
    }
  }

  private wakeNear(x: number, y: number, z: number, radius: number): void {
    const c = this.world.control;
    Atomics.store(c, C_EDIT_X, Math.round((x + 0.5) * PHYS_SCALE));
    Atomics.store(c, C_EDIT_Y, Math.round((y + 0.5) * PHYS_SCALE));
    Atomics.store(c, C_EDIT_Z, Math.round((z + 0.5) * PHYS_SCALE));
    Atomics.add(c, C_EDIT_EPOCH, 1);
    if (!this.workers) {
      this.core.wakeBodies(LAYOUT.bodies, this.settings.bodyCount, x + 0.5, y + 0.5, z + 0.5, radius);
    }
  }

  private emitHud(): void {
    const s = this.renderer.stats;
    const p = this.chunkProgress();
    this.onHud?.({
      fps: this.fps, frameMs: 1000 / Math.max(1, this.fps), cpuMs: this.cpuMs, gpuMs: 0,
      drawCalls: s.drawCalls, triangles: s.triangles, chunksDrawn: s.chunksDrawn, chunksCulled: s.chunksCulled,
      chunksReady: p.done, chunksTotal: p.total,
      instances: s.instances, physicsMs: this.physMs, workers: this.workers,
      x: this.player.pos[0], y: this.player.pos[1], z: this.player.pos[2], speed: this.player.speed,
      biome: this.biomeName(), clock: formatClock(this.timeOfDay),
      memMB: Math.round((performance as unknown as { memory?: { usedJSHeapSize: number } }).memory?.usedJSHeapSize ?? 0) / 1048576,
      flying: this.player.flying, onGround: this.player.onGround, inWater: this.player.inWater,
      selected: HOTBAR[this.selected].name, edits: this.edits, verts: this.totalVerts,
      target: this.hasHit,
    });
  }

  private biomeName(): string {
    const x = Math.floor(this.player.pos[0]), z = Math.floor(this.player.pos[2]);
    const y = Math.floor(this.player.pos[1]);
    if (y > SEA_LEVEL + 34) return 'Montanha';
    const top = this.world.getBlock(x, y - 1, z);
    if (top === 4) return 'Praia';
    if (top === 8) return 'Neve';
    if (y <= SEA_LEVEL + 1) return 'Oceano';
    return 'Campos';
  }

  /** Ações disparadas pelo HUD/teclas. */
  action(name: string): void {
    switch (name) {
      case 'respawn': this.player.respawn(); break;
      case 'fps': this.settings.fov = this.settings.fov > 80 ? 70 : 88; break;
      case 'spawnCube': this.bodies.spawnAtLook(this.eye, this.look); break;
      case 'explode': this.bodies.explode(this.player.pos[0], this.player.pos[1] + 1, this.player.pos[2]); break;
      case 'sel' + name: break;
      default: break;
    }
  }

  selectBlock(i: number): void { this.selected = clamp(i, 0, HOTBAR.length - 1) | 0; }

  dispose(): void {
    this.stop();
    this.pool?.shutdown();
    this.physWorker?.terminate();
  }
}

/** Utilitário: relógio in-game (0.5 = meio-dia). */
function formatClock(t: number): string {
  const totalMin = Math.floor(((t + 0.5) % 1) * 24 * 60);
  const h = Math.floor(totalMin / 60), m = totalMin % 60;
  return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}`;
}

/**
 * SISTEMA DE CORPOS RÍGIDOS (peças estilo "part" da Roblox)
 * A simulação roda no worker de física; aqui só há spawn, explosão e
 * partículas simples (que também são corpos com gravidade).
 */
export class BodySystem {
  private next = 0;
  private particles = 0;
  constructor(private eng: Engine) {}
  private get world(): SharedWorld { return this.eng.world; }

  /** Cria uma peça no ponto olhado (tecla G). */
  spawnAtLook(eye: Float32Array, dir: Float32Array): void {
    const i = this.alloc();
    if (i < 0) return;
    const x = eye[0] + dir[0] * 3.2, y = eye[1] + dir[1] * 3.2, z = eye[2] + dir[2] * 3.2;
    const colors = [0xff4d6dff, 0x37c6ffff, 0x39ff88ff, 0xffb020ff, 0xb06bffff];
    this.world.setBody(i, x, y, z, 0.45, colors[(Math.random() * colors.length) | 0], 1.5);
    this.kick(i, dir[0] * 12, 3.5, dir[2] * 12);
  }

  /** Explosão radial: acorda e arremessa tudo ao redor (tecla E). */
  explode(x: number, y: number, z: number): void {
    const count = this.eng.settings.bodyCount;
    const f = this.world.bodies, ci = this.world.bodiesI32;
    for (let i = 0; i < count; i++) {
      const o = i * 16;
      if (ci[o + 7] === 0) continue;
      const dx = f[o] - x, dy = f[o + 1] - y, dz = f[o + 2] - z;
      const d = Math.hypot(dx, dy, dz);
      if (d > 9) continue;
      const k = (1 - d / 9) * 16;
      ci[o + 7] &= ~4;                        // acorda
      f[o + 9] = 0;                           // zera repouso
      f[o + 3] += (dx / (d || 1)) * k;
      f[o + 4] += (dy / (d || 1)) * k + 4;
      f[o + 5] += (dz / (d || 1)) * k;
    }
  }

  /** Estilhaços ao quebrar bloco: corpos minúsculos que desaparecem. */
  puff(x: number, y: number, z: number, blockId: number): void {
    const color = BLOCK_COLORS[blockId] ?? 0x888888ff;
    for (let k = 0; k < 6; k++) {
      const i = this.alloc();
      if (i < 0) break;
      this.world.setBody(i, x, y, z, 0.09, color, 0);
      this.kick(i, (Math.random() - 0.5) * 5, 2 + Math.random() * 3, (Math.random() - 0.5) * 5);
      this.particles++;
    }
  }

  private alloc(): number {
    const count = this.eng.settings.bodyCount;
    const ci = this.world.bodiesI32;
    for (let n = 0; n < count; n++) {
      const i = (this.next + n) % count;
      if (ci[i * 16 + 7] === 0) { this.next = (i + 1) % count; return i; }
    }
    // Sem espaço: recicla o mais próximo do fim (o "mais velho")
    const i = this.next % Math.max(1, count);
    this.next = (i + 1) % Math.max(1, count);
    return i;
  }

  private kick(i: number, vx: number, vy: number, vz: number): void {
    const f = this.world.bodies, o = i * 16;
    f[o + 3] = vx; f[o + 4] = vy; f[o + 5] = vz;
    this.world.bodiesI32[o + 7] |= 1;
  }

  update(_dt: number): void {
    // Nada a fazer: a simulação acontece no worker. Aqui só limpamos
    // partículas antigas (vida curta) para não poluir a cena.
    if (this.particles > 300) this.particles = 0;
  }
}

/** Cores de estilhaço por bloco (formato RGBA empacotado big-endian visual). */
const BLOCK_COLORS: Record<number, number> = {
  1: 0x5fbf3fff, 2: 0x8a5f3aff, 3: 0x8f8f96ff, 4: 0xe0cf95ff, 5: 0x2f7fd0ff,
  6: 0x7b5a34ff, 7: 0x3f8f38ff, 8: 0xf2f6ffff, 9: 0xc08b4eff, 10: 0xa04a3cff,
  11: 0xbfe6f5ff, 12: 0x7f8076ff, 13: 0x9a8f80ff, 14: 0xa8d8ecff, 15: 0xffe08aff, 16: 0x3c3c46ff,
};

/** Detecção de SIMD por compilação de um módulo mínimo (custa ~0,2 ms). */
function detectSimd(): boolean {
  try {
    // (module (func (export "t") (param v128) (result v128) local.get 0))
    const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 1, 123, 1, 123, 3, 2, 1, 0, 7, 5, 1, 1, 116, 0, 0, 10, 6, 1, 4, 0, 32, 0, 11]);
    new WebAssembly.Module(bytes);
    return true;
  } catch { return false; }
}


