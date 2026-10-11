/**
 * ============================================================================
 *  VALIDAÇÃO DO ARQUIVO ÚNICO — simula o que o navegador faz em file://
 * ============================================================================
 *  Uso: npm run test:standalone
 *
 *  Confere os três pontos que só falhariam na mão do usuário:
 *   1) o WASM está realmente embutido (data: URL em base64, sem rede);
 *   2) o HTML não tem NENHUMA referência externa (nem script, nem imagem);
 *   3) o núcleo linka com memória COMUM — o único tipo que file:// permite —
 *      e gera/mesha um chunk sem worker.
 */

import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { SharedWorld, LAYOUT, chunkIndex } from '../src/core/memory';

const file = join(process.cwd(), 'public', 'downloads', 'roblox-max.html');
if (!existsSync(file)) {
  console.error('  ❌ arquivo único ausente — rode `npm run build:standalone`');
  process.exit(1);
}
const html = readFileSync(file, 'utf8');
let fails = 0;
const ok = (label: string, cond: boolean, extra = '') => {
  console.log(`  ${cond ? '\x1b[32mPASS\x1b[0m' : '\x1b[31mFAIL\x1b[0m'}  ${label}${extra ? '  ' + extra : ''}`);
  if (!cond) fails++;
};

ok('Arquivo autocontido (< 300 KB)', html.length < 300 * 1024, `${(html.length / 1024).toFixed(0)} KB`);
ok('CSS do Tailwind embutido', html.includes('<style>') && html.includes('.slot'));
ok('Bundle em script clássico (IIFE, sem import.meta)', !html.includes('import.meta'));

const refs = [...html.matchAll(/(?:src|href)="([^"]+)"/g)].map((m) => m[1]);
ok('Zero referências externas (nenhum request de rede)', refs.length === 0, refs.length ? refs.join(', ') : '100% offline');

const shared = /shared:\s*'(data:application\/wasm;base64,[^']+)'/.exec(html)?.[1];
const plain = /plain:\s*'(data:application\/wasm;base64,[^']+)'/.exec(html)?.[1];
ok('WASM embutido em data: base64 (os dois builds)', !!shared && !!plain,
   `${(((shared?.length ?? 0) + (plain?.length ?? 0)) / 1024).toFixed(0)} KB de base64`);

const main = async () => {
  // 1) O loader busca o núcleo com fetch() — inclusive de uma data: URL
  const res = await fetch(plain!);
  const bytes = await res.arrayBuffer();
  ok('fetch(data:application/wasm) funciona', res.ok && bytes.byteLength > 8000, `${bytes.byteLength} bytes`);

  // 2) Em file:// só existe memória comum: o binário "plain" precisa linkar
  const mem = new WebAssembly.Memory({ initial: 512, maximum: 512 });
  const world = new SharedWorld(mem, true, 20261011);
  ok('Memória comum aceita (sem SharedArrayBuffer)', world.isShared === false);

  // Com BufferSource, instantiate() devolve { module, instance } (e não a Instance direto).
  const { instance } = await WebAssembly.instantiate(bytes, {
    env: { memory: mem, abort: () => { throw new Error('abort do WASM'); } },
  });
  const core = instance.exports as unknown as { coreVersion(): number; generateChunk(...a: number[]): void;
    meshChunk(...a: number[]): number; surfaceY(h: number, x: number, z: number): number };
  ok('Núcleo linkado em memória comum', core.coreVersion() === 10007, `versão ${core.coreVersion()}`);

  // 3) Fluxo single-thread: gerar + meshar direto na "main thread"
  const t0 = performance.now();
  let verts = 0;
  let firstMs = 0;
  // Coluna completa de chunks (cy = 0..3): sem os 4, a coluna acima ficaria
  // "sólida virtual" (oclusão conservadora) e a superfície seria o topo do mundo.
  for (let cy = 0; cy < 4; cy++) {
    const slot = chunkIndex(6, cy, 6);
    core.generateChunk(world.headerPtr, slot, 6, cy, 6);
    world.table[slot] = slot;
    const v = core.meshChunk(world.headerPtr, slot, 6, cy, 6, world.meshPtr(0), LAYOUT.meshSlotBytes, world.arenaPtr(0));
    verts += v;
    if (cy === 0) firstMs = performance.now() - t0;
  }
  const ms = performance.now() - t0;
  ok('Geração + meshing sem workers (coluna de 4 chunks)', verts > 500,
     `${verts} vértices em ${ms.toFixed(2)} ms (1º chunk: ${firstMs.toFixed(2)} ms)`);
  const surface = core.surfaceY(world.headerPtr, 192, 192);
  ok('Mundo utilizável (spawn na superfície)', surface > 20 && surface < 90, `y=${surface}`);

  console.log(fails === 0
    ? '\n  \x1b[32m✅ arquivo único pronto para duplo clique\x1b[0m\n'
    : `\n  \x1b[31m${fails} falha(s)\x1b[0m\n`);
  process.exit(fails === 0 ? 0 : 1);
};
main().catch((e) => { console.error(e); process.exit(1); });
