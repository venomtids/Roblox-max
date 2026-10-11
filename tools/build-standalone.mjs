/**
 * ============================================================================
 *  BUILD DE ARQUIVO ÚNICO — "roblox-max.html"
 * ============================================================================
 *  Gera UM arquivo .html autocontido (~1 MB) que roda com duplo clique,
 *  sem servidor, sem Node, sem instalação de nada. É a resposta para quem
 *  não tem toolchain na máquina.
 *
 *  Como ele resolve as três limitações de `file://`:
 *
 *   1) SEM SERVIDOR → o bundle JS é convertido para IIFE (script clássico,
 *      que o navegador aceita em file://; módulo ES externo seria bloqueado
 *      por CORS).
 *   2) SEM HTTP → o binário WASM é embutido como `data:application/wasm;base64`
 *      e o CSS do Tailwind compilado vai inline. O jogo não faz UM único
 *      request de rede.
 *   3) SEM COOP/COEP → `SharedArrayBuffer` não existe em file://, então o
 *      build marca `__VX_FORCE_SINGLE_THREAD__`: o engine usa o núcleo
 *      `core-noshared.wasm` e gera o mundo na main thread, fatiado por frame.
 *      Tudo o mais (WebGPU, SIMD, física, edição, HUD) permanece idêntico.
 *
 *  Uso:  npm run build:standalone
 */

import { build } from 'esbuild';
import { readFileSync, writeFileSync, mkdirSync, readdirSync, existsSync } from 'node:fs';
import { execSync } from 'node:child_process';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'tools', '.build');
const DIST = join(ROOT, 'dist');
const OUT = join(ROOT, 'public', 'downloads', 'roblox-max.html');

const log = (m) => console.log(`  ${m}`);

mkdirSync(BUILD, { recursive: true });
mkdirSync(join(ROOT, 'public', 'downloads'), { recursive: true });

// ── 1. Núcleo WASM (os dois builds precisam existir) ───────────────────────
log('verificando os binários do núcleo WASM…');
for (const f of ['core.wasm', 'core-noshared.wasm']) {
  if (!existsSync(join(ROOT, 'public', 'wasm', f))) {
    throw new Error(`public/wasm/${f} não existe — rode "npm run wasm" antes.`);
  }
}
const sharedB64 = readFileSync(join(ROOT, 'public', 'wasm', 'core.wasm')).toString('base64');
const plainB64 = readFileSync(join(ROOT, 'public', 'wasm', 'core-noshared.wasm')).toString('base64');
log(`WASM embutido: ${((sharedB64.length + plainB64.length) / 1024).toFixed(0)} KB em base64`);

// ── 2. Build de produção (Vite) — só para obter JS + CSS já minificados ────
log('rodando o build de produção (Tailwind + TypeScript)…');
execSync('npx vite build', { cwd: ROOT, stdio: 'pipe' });

const assets = readdirSync(join(DIST, 'assets'));
const jsFile = assets.find((f) => f.startsWith('index-') && f.endsWith('.js'));
const cssFile = assets.find((f) => f.startsWith('index-') && f.endsWith('.css'));
if (!jsFile || !cssFile) throw new Error('saída do Vite inesperada em dist/assets');

// ── 3. ESM → IIFE (script clássico aceito em file://) ─────────────────────
log('convertendo o bundle ESM para IIFE (script clássico)…');
await build({
  entryPoints: [join(DIST, 'assets', jsFile)],
  bundle: true,
  format: 'iife',
  target: 'es2022',
  minify: true,
  legalComments: 'none',
  outfile: join(BUILD, 'app.iife.js'),
  logLevel: 'error',
});
const appJs = readFileSync(join(BUILD, 'app.iife.js'), 'utf8');
const css = readFileSync(join(DIST, 'assets', cssFile), 'utf8');

// ── 4. Monta o HTML único ────────────────────────────────────────────────
const prelude = `
// ── Ambiente de arquivo único: sem servidor, sem isolamento de origem ──
window.__VX_FORCE_SINGLE_THREAD__ = location.protocol === 'file:';
window.__VX_WASM__ = {
  shared: 'data:application/wasm;base64,${sharedB64}',
  plain:  'data:application/wasm;base64,${plainB64}',
};
`;

const html = `<!DOCTYPE html>
<html lang="pt-BR" class="dark">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
<meta name="theme-color" content="#07090f" />
<title>ROBLOX-MAX — arquivo único (offline)</title>
<meta name="description" content="Plataforma de jogos 3D voxel que roda 100% no navegador: WebGPU, WebAssembly SIMD, mundo procedural. Arquivo único, sem servidor." />
<style>${css}</style>
</head>
<body class="bg-vx-bg text-slate-200 font-game select-none overflow-hidden">

  <canvas id="gl" class="fixed inset-0 w-full h-full block bg-vx-bg"></canvas>
  <div class="pointer-events-none fixed inset-0 z-10 opacity-70"
       style="background:radial-gradient(circle at 50% 50%, transparent 52%, rgba(0,0,0,.55) 100%)"></div>
  <div id="hud" class="fixed inset-0 z-20 pointer-events-none"></div>

  <div id="boot" class="fixed inset-0 z-40 flex items-center justify-center bg-vx-bg/95 backdrop-blur-sm">
    <div class="w-[min(92vw,660px)] rounded-2xl border border-vx-line bg-vx-panel shadow-panel p-7">
      <div class="flex items-center gap-4">
        <div class="grid h-14 w-14 place-items-center rounded-xl bg-gradient-to-br from-vx-accent to-vx-accent2 shadow-glow">
          <span class="text-2xl font-bold text-black">R</span>
        </div>
        <div class="leading-tight">
          <h1 class="text-2xl font-bold tracking-wide">ROBLOX<span class="text-vx-accent">-MAX</span></h1>
          <p class="text-xs uppercase tracking-[0.22em] text-slate-400">Arquivo único · offline · WebGPU + WASM</p>
        </div>
      </div>
      <div id="boot-steps" class="mt-6 space-y-2 text-sm"></div>
      <div class="mt-6">
        <div class="h-2.5 w-full overflow-hidden rounded-full bg-black/50 border border-vx-line">
          <div id="boot-bar" class="h-full w-0 bg-gradient-to-r from-vx-accent to-vx-accent2 transition-[width] duration-200"></div>
        </div>
        <div class="mt-2 flex justify-between font-mono text-[11px] text-slate-400">
          <span id="boot-msg">inicializando…</span><span id="boot-pct">0%</span>
        </div>
      </div>
      <div class="mt-6 grid grid-cols-2 gap-3 sm:grid-cols-4" id="boot-badges"></div>
      <button id="boot-play" disabled
        class="mt-7 w-full rounded-xl bg-gradient-to-r from-vx-accent to-vx-accent2 py-4 text-lg font-bold uppercase tracking-widest text-black shadow-glow transition hover:brightness-110 disabled:cursor-not-allowed disabled:from-slate-700 disabled:to-slate-700 disabled:text-slate-500 disabled:shadow-none">
        Carregando mundo…
      </button>
      <p class="mt-3 text-center text-[11px] text-slate-500">
        WASD mover · Espaço pular · Shift correr · V voar · Clique esq. quebrar · Clique dir. colocar · G peça · E explosão · F3 painel
      </p>
      <p class="mt-2 text-center text-[10px] text-slate-600">
        Executando direto de um arquivo .html (sem servidor): geração do mundo na main thread.
        Para multithreading real com 8 núcleos, sirva a versão web com COOP/COEP.
      </p>
    </div>
  </div>

  <div id="fatal" class="fixed inset-0 z-50 hidden items-center justify-center bg-black/90 p-6">
    <div class="max-w-xl rounded-2xl border border-vx-danger/60 bg-vx-panel p-7 shadow-panel">
      <h2 class="text-xl font-bold text-vx-danger">Não foi possível iniciar</h2>
      <p id="fatal-msg" class="mt-3 text-sm text-slate-300"></p>
      <div id="fatal-help" class="mt-4 rounded-lg border border-vx-line bg-black/40 p-4 font-mono text-[12px] leading-relaxed text-slate-400"></div>
    </div>
  </div>

<script>${prelude}</script>
<script>${appJs}</script>
</body>
</html>
`;

writeFileSync(OUT, html);
const kb = (Buffer.byteLength(html) / 1024).toFixed(0);
console.log(`\n  ✅ arquivo único gerado: ${OUT.replace(ROOT + '/', '')} (${kb} KB)`);
console.log('     Baixe, dê duplo clique e clique em JOGAR — sem Node, sem Python, sem servidor.\n');
