/**
 * ============================================================================
 *  ROBLOX-MAX · bootstrap do site
 * ============================================================================
 *  Sequência de arranque (o que o usuário vê na tela de carregamento):
 *    1. Validação do ambiente (WebGPU, SharedArrayBuffer, COOP/COEP, SIMD)
 *    2. Criação da engine (device, WASM, workers, pipelines, atlas)
 *    3. Geração+mesh paralela dos 576 chunks (barra de progresso real)
 *    4. Hot-swap para o jogo: HUD, pointer lock e loop a 60+ FPS
 *
 *  A tela de carregamento não é cosmética: cada linha corresponde a uma
 *  capacidade real detectada no navegador do usuário.
 */

import './style.css';
import { Engine, DEFAULT_SETTINGS } from './game/engine';
import { Hud } from './ui/hud';
import { diagnose, suggestQuality, type BrowserReport } from './game/browser-support';

const boot = document.getElementById('boot')!;
const bootSteps = document.getElementById('boot-steps')!;
const bootBar = document.getElementById('boot-bar')!;
const bootMsg = document.getElementById('boot-msg')!;
const bootPct = document.getElementById('boot-pct')!;
const bootPlay = document.getElementById('boot-play') as HTMLButtonElement;
const bootBadges = document.getElementById('boot-badges')!;
const fatal = document.getElementById('fatal')!;
const fatalMsg = document.getElementById('fatal-msg')!;
const fatalHelp = document.getElementById('fatal-help')!;

const ICONS: Record<string, string> = { ok: '✅', warn: '⚠️', fail: '⛔', run: '⏳' };
const stepNodes = new Map<string, HTMLElement>();

function stage(label: string, state: 'ok' | 'warn' | 'fail' | 'run', detail?: string): void {
  let node = stepNodes.get(label);
  if (!node) {
    node = document.createElement('div');
    node.className = 'flex items-start gap-2 font-mono text-[12px]';
    bootSteps.appendChild(node);
    stepNodes.set(label, node);
  }
  node.innerHTML = `
    <span class="w-5 shrink-0">${ICONS[state]}</span>
    <span class="text-slate-200">${label}</span>
    <span class="ml-auto text-right text-slate-500">${detail ?? ''}</span>`;
}

function progress(frac: number, label: string): void {
  const pct = Math.max(0, Math.min(1, frac));
  bootBar.style.width = `${(pct * 100).toFixed(1)}%`;
  bootPct.textContent = `${(pct * 100).toFixed(0)}%`;
  bootMsg.textContent = label;
}

function badge(label: string, value: string, tone = 'accent'): void {
  const color = tone === 'warn' ? 'text-vx-warn' : tone === 'fail' ? 'text-vx-danger' : 'text-vx-accent';
  const el = document.createElement('div');
  el.className = 'rounded-lg border border-vx-line bg-black/40 px-3 py-2';
  el.innerHTML = `<div class="text-[10px] uppercase tracking-widest text-slate-500">${label}</div>
                  <div class="font-mono text-[12px] ${color}">${value}</div>`;
  bootBadges.appendChild(el);
}

function showFatal(message: string, help?: string): void {
  fatal.classList.remove('hidden');
  fatal.classList.add('flex');
  fatalMsg.textContent = message;
  fatalHelp.innerHTML = help ?? `
    <div>1) Use um navegador com WebGPU: <b>Chrome 113+</b>, <b>Edge 113+</b> ou <b>Chrome no Android 121+</b>.</div>
    <div>2) Em Linux, habilite em <b>chrome://flags/#enable-unsafe-webgpu</b> (ou use uma build recente).</div>
    <div>3) Firefox: <b>dom.webgpu.enabled</b> + <b>gfx.webgpu.force-enabled</b>.</div>
    <div>4) O servidor precisa enviar <b>COOP/COEP</b> — este projeto já configura isso (vite.config.ts).</div>
  `;
}

/**
 * Checagem de ambiente + diagnóstico do navegador ANTES de tocar em WebGPU.
 * Cada item ruim vem com a instrução exata de como resolver (por navegador):
 * é a diferença entre "não funciona" e "ligue esta opção e recarregue".
 */
function envChecks(): BrowserReport {
  const report = diagnose();
  document.title = `ROBLOX-MAX · ${report.browserName}`;

  for (const item of report.items) {
    stage(item.label, item.ok ? 'ok' : item.critical ? 'fail' : 'warn', item.detail);
    if (!item.ok && item.fix) {
      const help = document.createElement('div');
      help.className = 'ml-7 mb-1 rounded-lg border border-vx-warn/40 bg-vx-warn/5 p-2 font-mono text-[11px] leading-relaxed text-vx-warn/90';
      help.textContent = `→ ${item.fix}`;
      bootSteps.appendChild(help);
    }
  }

  badge('Navegador', report.browserName);
  badge('WebGPU', report.webgpu ? 'disponível' : 'ausente', report.webgpu ? 'accent' : 'fail');
  badge('Threads', report.sab && report.isolated ? `${Math.max(1, report.cores - 1)} núcleos` : 'single-thread',
        report.sab && report.isolated ? 'accent' : 'warn');
  badge('Núcleos / RAM', `${report.cores}${report.deviceMemoryGB ? ' / ' + report.deviceMemoryGB + 'GB' : ''}`);

  if (report.blocked) {
    showFatal(report.summary, report.items.find((i) => i.critical && !i.ok)?.fix
      ?.split('  ·  ').map((line) => `<div>• ${line}</div>`).join(''));
  }
  return report;
}

async function run(): Promise<void> {
  const report = envChecks();
  if (report.blocked) return;
  const canvas = document.getElementById('gl') as HTMLCanvasElement;
  const quality = suggestQuality(report);

  try {
    progress(0.02, `Criando engine · perfil ${quality.tier}…`);
    const engine = await Engine.create(canvas, {
      stage,
      progress,
      ready: () => { /* apenas o contrato */ },
      fatal: showFatal,
    }, 20261011, quality);

    progress(0.12, 'Preparando o mundo…');
    await engine.loadWorld();

    // ── HUD + hot-swap ─────────────────────────────────────────────────
    const hud = new Hud(engine);
    engine.onHud = (info) => hud.update(info);

    badge('Workers', String(engine.workers));
    badge('Perfil', `${quality.tier} · MSAA ${quality.msaa}× · ${quality.renderScale.toFixed(2)}×`, quality.tier === 'alto' ? 'accent' : 'warn');
    badge('Compilação', `${engine.buildMs.toFixed(0)} ms`);

    engine.start();
    bootPlay.disabled = false;
    bootPlay.textContent = '▶ JOGAR';
    bootPlay.onclick = () => {
      boot.style.display = 'none';
      engine.input.requestLock();
      hud.toast('Clique para capturar o mouse · F3 telemetria');
    };
    setTimeout(() => { if (boot.style.display !== 'none') bootPlay.focus(); }, 50);

    // Atalho: Enter também inicia
    addEventListener('keydown', (e: KeyboardEvent) => {
      if (e.code === 'Enter' && boot.style.display !== 'none') bootPlay.click();
    });
  } catch (err) {
    const e = err as Error;
    stage('Falha na inicialização', 'fail', e.message);
    showFatal(e.message);
  }
}

// Exposto para depuração no console do navegador
(globalThis as unknown as { VX_SETTINGS: typeof DEFAULT_SETTINGS }).VX_SETTINGS = DEFAULT_SETTINGS;

run();
