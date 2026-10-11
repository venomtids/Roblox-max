/**
 * ============================================================================
 *  TESTES DA CENTRAL DE INSTALAÇÃO
 * ============================================================================
 *  Uso: npm run test:install-hub
 *
 *  A lógica de decisão (UA → plataforma → plano) é pura de propósito: assim é
 *  testável sem navegador, e um erro de mapeamento (dizer a um usuário de
 *  Steam Deck que ele tem "Linux genérico", por exemplo) aparece aqui em vez
 *  de aparecer para o usuário.
 */

import {
  platformFromUA, planFor, BROWSER_VM_BLOCKERS,
  type Platform,
} from '../src/ui/install-hub';

let pass = 0; let fail = 0;
const ok = (label: string, cond: boolean, extra = '') => {
  if (cond) { console.log(`  \x1b[32mPASS\x1b[0m  ${label}${extra ? '  ' + extra : ''}`); pass++; }
  else { console.log(`  \x1b[31mFAIL\x1b[0m  ${label}${extra ? '  ' + extra : ''}`); fail++; }
};

// ── User-Agents reais (recortados do essencial) ────────────────────────────
const UAS: Array<[string, Platform]> = [
  ['Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/141.0.0.0 Safari/537.36', 'windows'],
  ['Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 Safari/605.1.15', 'macos'],
  ['Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/141.0.0.0 Safari/537.36', 'linux'],
  ['Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:145.0) Gecko/20100101 Firefox/145.0', 'linux'],
  ['Mozilla/5.0 (X11; Linux x86_64; Valve Steam Deck) AppleWebKit/537.36 Chrome/120.0.0.0 Safari/537.36', 'steamdeck'],
  ['Mozilla/5.0 (X11; CrOS x86_64 14541.0.0) AppleWebKit/537.36 Chrome/120.0.0.0 Safari/537.36', 'chromeos'],
  ['Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 Chrome/141.0.0.0 Mobile Safari/537.36', 'android'],
  ['Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 Mobile/15E148', 'ios'],
  ['curl/8.5.0', 'unknown'],
  ['', 'unknown'],
];

console.log('\n\x1b[1mCENTRAL DE INSTALAÇÃO · testes\x1b[0m\n');
console.log('\x1b[1m1. Detecção de plataforma pelo User-Agent\x1b[0m');
let uaOk = 0;
for (const [ua, expected] of UAS) {
  const got = platformFromUA(ua);
  const label = `${expected.padEnd(10)} ← ${(ua.split(')')[0] + ')').slice(0, 52) || '(vazio)'}`;
  if (got === expected) { uaOk++; ok(label, true); } else { ok(label + ` (obteve ${got})`, false); }
}
ok(`todas as ${UAS.length} assinaturas reconhecidas`, uaOk === UAS.length, `${uaOk}/${UAS.length}`);

console.log('\n\x1b[1m2. Coerência dos planos por plataforma\x1b[0m');
const ALL: Platform[] = ['windows', 'macos', 'linux', 'steamdeck', 'chromeos', 'android', 'ios', 'unknown'];
for (const p of ALL) {
  const plan = planFor(p);
  const okPlan = plan.platform === p && plan.steps.length > 0 && plan.badge.length > 0
    && plan.headline.length > 10 && plan.summary.length > 20
    && plan.steps.every((s) => s.title.length > 0 && s.detail.length > 10);
  ok(`plano de "${p}" completo` , okPlan, `${plan.steps.length} passos`);
}

console.log('\n\x1b[1m3. Precisão técnica dos planos\x1b[0m');
const win = planFor('windows');
ok('Windows aponta o instalador OFICIAL da Roblox',
   win.steps.some((s) => s.link === 'https://www.roblox.com/download'));
ok('Windows avisa contra "FPS unlocker" pirata',
   win.steps.some((s) => (s.warn ?? '').toLowerCase().includes('ban')));

const lin = planFor('linux');
ok('Linux oferece o script deste projeto',
   lin.steps.some((s) => s.link === '/downloads/roblox-linux.sh'));
ok('Linux manda rodar o doctor antes de instalar',
   lin.steps.some((s) => (s.code ?? '').includes('doctor')));
ok('Linux avisa que Wine/Proton estão mortos',
   lin.steps.some((s) => (s.warn ?? '').includes('Wine')));
ok('Linux oferece --dry-run (simular sem alterar)',
   lin.steps.some((s) => (s.code ?? '').includes('--dry-run')));

const deck = planFor('steamdeck');
ok('Steam Deck explica Modo Desktop → Modo Jogo',
   deck.summary.toLowerCase().includes('desktop') || deck.steps.some((s) => s.title.toLowerCase().includes('desktop')));

const chro = planFor('chromeos');
ok('ChromeOS aponta o app Android (não Linux) como caminho principal',
   chro.steps[0].title.toLowerCase().includes('play') || chro.steps[0].detail.toLowerCase().includes('android'));

console.log('\n\x1b[1m4. Bloqueadores da VM de navegador\x1b[0m');
ok('exatamente 4 bloqueadores independentes', BROWSER_VM_BLOCKERS.length === 4);
ok('cobre arquitetura 32 vs 64 bits',
   BROWSER_VM_BLOCKERS.some((b) => b.title.includes('32-bit') && b.detail.includes('x86-64')));
ok('cobre ausência de GPU/Vulkan',
   BROWSER_VM_BLOCKERS.some((b) => b.title.toLowerCase().includes('gpu') && b.detail.includes('Vulkan')));
ok('cobre anti-cheat, ToS e risco de banimento',
   BROWSER_VM_BLOCKERS.some((b) => {
     const full = `${b.title} ${b.detail}`;
     return full.includes('Hyperion') && (full.includes('Termos de Uso') || full.includes('ToS')) && /banid|banida|ban/.test(full);
   }));
ok('cobre desempenho com número (1–5%)',
   BROWSER_VM_BLOCKERS.some((b) => b.detail.includes('1–5%')));
ok('nenhum bloqueador promete contorno/macete',
   !BROWSER_VM_BLOCKERS.some((b) => /bypass|contornar|burlar/i.test(b.detail)));

console.log('\n\x1b[1m5. Alternativas oferecidas\x1b[0m');
for (const p of ALL) {
  const plan = planFor(p);
  const hasGame = plan.alternatives.some((a) => a.href.includes('roblox-max.html'));
  const hasLinux = plan.alternatives.some((a) => a.href.includes('roblox-linux.sh'));
  ok(`"${p}" oferece o jogo do site e o instalador Linux`, hasGame && hasLinux);
}

console.log(`\n\x1b[1mResultado:\x1b[0m ${fail === 0 ? `\x1b[32m${pass} testes, tudo verde\x1b[0m` : `\x1b[31m${fail} falhas\x1b[0m de ${pass + fail}`}\n`);
process.exit(fail === 0 ? 0 : 1);
