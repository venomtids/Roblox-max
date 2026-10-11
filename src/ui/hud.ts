/**
 * ============================================================================
 *  HUD / INTERFACE GAMER (Tailwind CSS)
 * ============================================================================
 *  Construída como HTML+Tailwind direto no DOM — zero framework, zero
 *  virtual-dom, zero custo de reconciliação por frame. As atualizações são
 *  cirúrgicas: `textContent` em ~10 nós, a 5 Hz.
 *
 *  Componentes:
 *   • painel de telemetria (FPS, ms de CPU/GPU, draw calls, triângulos)
 *   • chips de status (workers, GPU, direção do olhar, bioma/relógio)
 *   • hotbar de blocos (9 slots, seleção por teclado/scroll)
 *   • menu de configurações (sliders ligados à engine em tempo real)
 *   • ajuda de controles recolhível
 */

import type { Engine, HudInfo } from '../game/engine';
import { HOTBAR } from '../game/engine';

export class Hud {
  private root: HTMLElement;
  private showDebug = true;
  private menuOpen = false;
  private nodes: Record<string, HTMLElement> = {};

  constructor(private engine: Engine) {
    this.root = document.getElementById('hud')!;
    this.build();
    this.bindKeys();
  }

  private build(): void {
    this.root.innerHTML = `
      <!-- crosshair -->
      <div class="crosshair" id="hud-cross"></div>

      <!-- telemetria -->
      <div id="hud-debug" class="pointer-events-none absolute left-3 top-3 panel p-3 font-mono text-[11px] leading-relaxed text-slate-300 min-w-[228px]">
        <div class="mb-2 flex items-center justify-between gap-2">
          <span class="text-vx-accent font-bold tracking-widest">TELEMETRIA</span>
          <span id="hud-fps" class="chip border-vx-accent/40 text-vx-accent">-- FPS</span>
        </div>
        <div id="hud-lines" class="space-y-0.5"></div>
      </div>

      <!-- status -->
      <div class="pointer-events-none absolute right-3 top-3 flex flex-col items-end gap-2">
        <div class="panel px-3 py-2 flex items-center gap-2">
          <span class="h-2 w-2 rounded-full bg-vx-accent animate-pulseSlow"></span>
          <span class="text-[11px] font-bold tracking-widest text-slate-300">ROBLOX<span class="text-vx-accent">-MAX</span></span>
          <span id="hud-gpu" class="chip max-w-[220px] truncate">GPU</span>
        </div>
        <div class="flex flex-wrap justify-end gap-2 max-w-[320px]">
          <span id="hud-threads" class="chip">threads</span>
          <span id="hud-chunks" class="chip">chunks</span>
          <span id="hud-clock" class="chip">--:--</span>
          <span id="hud-biome" class="chip">---</span>
        </div>
        <div class="flex gap-2 pointer-events-auto">
          <button id="hud-btn-menu" class="btn">Menu (ESC)</button>
          <button id="hud-btn-debug" class="btn">F3</button>
        </div>
      </div>

      <!-- hotbar -->
      <div class="pointer-events-auto absolute bottom-4 left-1/2 -translate-x-1/2">
        <div id="hud-hotbar" class="panel flex items-center gap-1.5 p-2"></div>
        <div class="mt-1 text-center font-mono text-[10px] uppercase tracking-widest text-slate-500">
          <span id="hud-sel">Grama</span> · scroll ou 1-9 · G peça · E explosão · R respawn
        </div>
      </div>

      <!-- ajuda -->
      <div class="pointer-events-none absolute bottom-4 left-3 panel p-3 text-[11px] leading-snug text-slate-400 max-w-[260px] hidden md:block">
        <div class="mb-1 font-bold tracking-widest text-slate-300">CONTROLES</div>
        <div><b class="text-slate-200">WASD</b> mover · <b class="text-slate-200">Espaço</b> pular</div>
        <div><b class="text-slate-200">Shift</b> correr · <b class="text-slate-200">Ctrl</b> agachar</div>
        <div><b class="text-slate-200">V</b> voar · <b class="text-slate-200">G</b> soltar peça</div>
        <div><b class="text-slate-200">Clique</b> esq. quebrar / dir. colocar</div>
        <div><b class="text-slate-200">N</b> noite/dia acelerado · <b class="text-slate-200">R</b> respawn</div>
      </div>

      <!-- velocidade -->
      <div class="pointer-events-none absolute bottom-24 left-1/2 -translate-x-1/2 hidden" id="hud-speed-wrap">
        <div class="panel px-3 py-1 font-mono text-[11px] text-vx-accent" id="hud-speed">0.0 m/s</div>
      </div>

      <!-- MENU -->
      <div id="hud-menu" class="pointer-events-auto absolute inset-0 hidden items-center justify-center bg-black/60 backdrop-blur-sm">
        <div class="panel w-[min(94vw,620px)] p-6 max-h-[92vh] overflow-auto">
          <div class="flex items-center justify-between">
            <h2 class="text-xl font-bold tracking-wide">CONFIGURAÇÕES</h2>
            <button id="menu-close" class="btn">Fechar</button>
          </div>
          <p class="mt-1 text-[11px] uppercase tracking-widest text-slate-500">Ajustes aplicados em tempo real</p>
          <div id="menu-body" class="mt-5 space-y-4"></div>
          <div class="mt-6 flex flex-wrap gap-2">
            <button id="menu-respawn" class="btn primary">Respawn</button>
            <button id="menu-cube" class="btn">Soltar peça (G)</button>
            <button id="menu-boom" class="btn danger">Explosão (E)</button>
            <button id="menu-night" class="btn">Alternar dia/noite</button>
          </div>
          <div class="mt-5 rounded-lg border border-vx-line bg-black/40 p-3 font-mono text-[11px] text-slate-400">
            <div>Dica: clique na tela para capturar o mouse (pointer lock).</div>
            <div>F3 esconde a telemetria · render scale &lt; 1.0 ajuda em GPUs integradas.</div>
          </div>
        </div>
      </div>

      <!-- toast -->
      <div id="hud-toast" class="pointer-events-none absolute left-1/2 top-24 -translate-x-1/2 opacity-0 transition-opacity duration-300">
        <div class="panel px-4 py-2 text-sm" id="hud-toast-text"></div>
      </div>
    `;

    // Slots da hotbar
    const bar = document.getElementById('hud-hotbar')!;
    bar.innerHTML = HOTBAR.map((b, i) => `
      <button class="slot" data-slot="${i}" data-active="${i === 0}">
        <span class="kb">${b.key}</span>
        <span class="sw" style="background:${b.color}"></span>
      </button>`).join('');
    bar.querySelectorAll<HTMLButtonElement>('.slot').forEach((el) => {
      el.addEventListener('click', () => {
        this.engine.selectBlock(Number(el.dataset.slot));
        this.syncHotbar();
      });
    });

    // Sliders (configurações)
    const s = this.engine.settings;
    const sliders: Array<[string, string, number, number, number, (v: number) => void]> = [
      ['Escala de renderização', 'renderScale', 0.5, 1.5, 0.05, (v) => (s.renderScale = v)],
      ['Campo de visão', 'fov', 60, 105, 1, (v) => (s.fov = v)],
      ['Distância de visão', 'viewDistance', 96, 512, 16, (v) => (s.viewDistance = v)],
      ['Velocidade do dia', 'daySpeed', 0, 1 / 30, 1 / 600, (v) => (s.daySpeed = v)],
      ['Sensibilidade', 'sens', 0.2, 3, 0.05, (v) => { s.sensitivity = v; this.engine.input.sensitivity = v; }],
      ['Peças ativas (corpos)', 'bodyCount', 100, 4096, 50, (v) => (s.bodyCount = v | 0)],
    ];
    const menuBody = document.getElementById('menu-body')!;
    menuBody.innerHTML = sliders.map(([label, id, min, max, step]) => `
      <label class="block">
        <div class="flex justify-between text-[11px] uppercase tracking-widest text-slate-400">
          <span>${label}</span><span class="font-mono text-vx-accent" id="val-${id}"></span>
        </div>
        <input id="sl-${id}" type="range" min="${min}" max="${max}" step="${step}" class="mt-2" />
      </label>`).join('');
    for (const [, id, , , step, apply] of sliders) {
      const el = document.getElementById(`sl-${id}`) as HTMLInputElement;
      const val = document.getElementById(`val-${id}`)!;
      const current = (s as unknown as Record<string, number>)[id === 'sens' ? 'sensitivity' : id];
      el.value = String(current);
      val.textContent = fmt(current, step);
      el.addEventListener('input', () => {
        const v = Number(el.value);
        apply(v);
        val.textContent = fmt(v, step);
      });
    }

    // Botões
    const on = (id: string, fn: () => void) => document.getElementById(id)!.addEventListener('click', fn);
    on('hud-btn-menu', () => this.toggleMenu(true));
    on('menu-close', () => this.toggleMenu(false));
    on('hud-btn-debug', () => this.toggleDebug());
    on('menu-respawn', () => { this.engine.player.respawn(); this.toast('Respawn'); });
    on('menu-cube', () => this.engine.action('spawnCube'));
    on('menu-boom', () => this.engine.action('explode'));
    on('menu-night', () => { this.engine.settings.daySpeed = this.engine.settings.daySpeed > 0 ? 0 : 1 / 30; });

    this.nodes = {
      fps: document.getElementById('hud-fps')!,
      lines: document.getElementById('hud-lines')!,
      gpu: document.getElementById('hud-gpu')!,
      threads: document.getElementById('hud-threads')!,
      chunks: document.getElementById('hud-chunks')!,
      clock: document.getElementById('hud-clock')!,
      biome: document.getElementById('hud-biome')!,
      sel: document.getElementById('hud-sel')!,
      toast: document.getElementById('hud-toast')!,
      toastText: document.getElementById('hud-toast-text')!,
      debug: document.getElementById('hud-debug')!,
      menu: document.getElementById('hud-menu')!,
      speedWrap: document.getElementById('hud-speed-wrap')!,
      speed: document.getElementById('hud-speed')!,
      cross: document.getElementById('hud-cross')!,
    };
    this.nodes.gpu.textContent = this.engine.adapterName;
    this.nodes.threads.textContent = this.engine.workers ? `${this.engine.workers} workers` : 'single-thread';
    this.nodes.speedWrap.classList.remove('hidden');
  }

  private bindKeys(): void {
    this.engine.input.onKey((code, down) => {
      if (!down) return;
      if (code === 'Escape') { this.toggleMenu(!this.menuOpen); return; }
      if (code === 'F3') { this.toggleDebug(); return; }
      if (code === 'KeyR') { this.engine.player.respawn(); this.toast('Respawn'); return; }
      if (code === 'KeyG') { this.engine.action('spawnCube'); return; }
      if (code === 'KeyE') { this.engine.action('explode'); this.toast('💥 Explosão'); return; }
      if (code === 'KeyN') {
        const s = this.engine.settings;
        s.daySpeed = s.daySpeed > 0 ? 0 : 1 / 20;
        this.toast(s.daySpeed > 0 ? '☀️ Ciclo dia/noite rápido' : '⏸️ Tempo congelado');
        return;
      }
      if (code === 'KeyF') { this.engine.settings.fov = this.engine.settings.fov > 80 ? 70 : 95; this.toast(`FOV ${this.engine.settings.fov}°`); return; }
      // 1-9 seleciona bloco
      const m = /^Digit([1-9])$/.exec(code);
      if (m) { this.engine.selectBlock(Number(m[1]) - 1); this.syncHotbar(); return; }
      // P muda a paleta/gpu probe
      if (code === 'KeyP') { this.engine.settings.renderScale = this.engine.settings.renderScale < 0.85 ? 1 : 0.7; this.toast(`Render scale ${this.engine.settings.renderScale.toFixed(2)}×`); }
    });
  }

  private syncHotbar(): void {
    const bar = document.getElementById('hud-hotbar')!;
    bar.querySelectorAll<HTMLElement>('.slot').forEach((el, i) => {
      el.dataset.active = String(i === this.engine.selected);
    });
    this.nodes.sel.textContent = HOTBAR[this.engine.selected].name;
  }

  toggleDebug(): void {
    this.showDebug = !this.showDebug;
    this.nodes.debug.style.display = this.showDebug ? '' : 'none';
  }

  toggleMenu(open: boolean): void {
    this.menuOpen = open;
    this.nodes.menu.style.display = open ? 'flex' : 'none';
    this.nodes.menu.classList.toggle('hidden', !open);
    if (open && document.pointerLockElement) document.exitPointerLock();
  }

  toast(msg: string): void {
    this.nodes.toastText.textContent = msg;
    this.nodes.toast.style.opacity = '1';
    clearTimeout(this.toastTimer);
    this.toastTimer = setTimeout(() => { this.nodes.toast.style.opacity = '0'; }, 1400);
  }
  private toastTimer: number | undefined;

  /** Atualização periódica (chamada pela engine a 5 Hz). */
  update(info: HudInfo): void {
    const f = this.nodes;
    f.fps.textContent = `${info.fps.toFixed(0)} FPS`;
    f.fps.className = 'chip ' + (info.fps >= 55 ? 'border-vx-accent/50 text-vx-accent'
      : info.fps >= 30 ? 'border-vx-warn/50 text-vx-warn' : 'border-vx-danger/60 text-vx-danger');
    f.chunks.textContent = `${info.chunksReady}/${info.chunksTotal} chunks`;
    f.clock.textContent = `${info.clock} ${info.biome}`;
    f.biome.textContent = info.flying ? 'VOANDO' : info.inWater ? 'NA ÁGUA' : info.onGround ? 'NO CHÃO' : 'NO AR';
    f.speed.textContent = `${info.speed.toFixed(1)} m/s`;
    const fpsOk = info.frameMs < 16.7;
    f.lines.innerHTML = `
      <div>frame <b class="text-slate-100">${info.frameMs.toFixed(1)} ms</b> · cpu <b class="${info.cpuMs < 6 ? 'text-vx-accent' : 'text-vx-warn'}">${info.cpuMs.toFixed(2)} ms</b>${fpsOk ? '' : ' <span class="text-vx-warn">(gargalo)</span>'}</div>
      <div>draw calls <b class="text-slate-100">${info.drawCalls}</b> · tri <b class="text-slate-100">${formatK(info.triangles)}</b></div>
      <div>chunks <b class="text-slate-100">${info.chunksDrawn}</b> vis / <b class="text-slate-100">${info.chunksCulled}</b> cortados</div>
      <div>física <b class="text-slate-100">${info.physicsMs.toFixed(2)} ms</b> · corpos <b class="text-slate-100">${info.instances}</b></div>
      <div>threads <b class="text-slate-100">${info.workers}</b> · vértices <b class="text-slate-100">${formatK(info.verts)}</b></div>
      <div>x <b class="text-slate-100">${info.x.toFixed(1)}</b> y <b class="text-slate-100">${info.y.toFixed(1)}</b> z <b class="text-slate-100">${info.z.toFixed(1)}</b></div>
      <div>bloco <b class="text-slate-100">${info.selected}</b> · edições <b class="text-slate-100">${info.edits}</b></div>
      <div class="pt-1 text-slate-500">WebGPU · WASM SIMD · SharedArrayBuffer</div>
    `;
    // crosshair "acerta" quando há bloco sob a mira
    f.cross.classList.toggle('hit', info.target);
  }

  setStageDetail(detail: string): void { this.nodes.gpu.textContent = detail; }
}

function formatK(n: number): string {
  if (n >= 1e6) return (n / 1e6).toFixed(2) + 'M';
  if (n >= 1e3) return (n / 1e3).toFixed(1) + 'k';
  return String(n | 0);
}

function fmt(v: number, step: number): string {
  if (step < 0.01) return v.toFixed(4);
  if (step < 0.1) return v.toFixed(2);
  return v.toFixed(step >= 1 ? 0 : 1);
}
