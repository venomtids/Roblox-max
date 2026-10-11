/**
 * Verifica que os dois builds do núcleo existem, são válidos e — o mais
 * importante — que cada um LINKA com o tipo de memória correspondente.
 * Um mismatch aqui só apareceria no navegador do usuário: melhor falhar aqui.
 */
import { readFileSync, statSync } from 'node:fs';

const cases = [
  { file: 'public/wasm/core.wasm', shared: true, label: 'multithread (shared)' },
  { file: 'public/wasm/core-noshared.wasm', shared: false, label: 'compatível (single-thread)' },
];
let fail = 0;
for (const c of cases) {
  const bytes = readFileSync(c.file);
  const mod = new WebAssembly.Module(bytes);
  const memory = new WebAssembly.Memory({ initial: 512, maximum: 512, shared: c.shared });
  try {
    new WebAssembly.Instance(mod, { env: { memory, abort() {} } });
    console.log(`  ✅ ${c.file.padEnd(34)} ${String(statSync(c.file).size).padStart(6)} B  · ${c.label}`);
  } catch (e) {
    console.log(`  ❌ ${c.file.padEnd(34)} link falhou: ${e.message}`);
    fail = 1;
  }
}
process.exit(fail);
