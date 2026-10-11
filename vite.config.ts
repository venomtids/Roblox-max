import { defineConfig } from 'vite';

/**
 * ROBLOX-MAX · Configuração de build
 * ────────────────────────────────────────────────────────────────────────
 * 1) COOP/COEP: SharedArrayBuffer (e portanto `crossOriginIsolated`) só é
 *    habilitado quando a página é servida com estes dois headers. Eles são a
 *    chave do multithreading compartilhado (WASM + N workers no MESMO heap).
 * 2) Workers em ESM (`format: 'es'`) para permitir `import` dos módulos TS
 *    dentro dos workers sem bundlar nada na mão.
 * 3) `target: esnext` → o output não é transpilado para baixo: queremos
 *    Atomics, BigInt, top-level await e WebGPU intactos.
 */
const crossOriginIsolation = {
  name: 'roblox-max-isolation',
  configureServer(server: any) {
    server.middlewares.use((_req: any, res: any, next: any) => {
      res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
      res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
      res.setHeader('Cross-Origin-Resource-Policy', 'cross-origin');
      next();
    });
  },
  configurePreviewServer(server: any) {
    server.middlewares.use((_req: any, res: any, next: any) => {
      res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
      res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
      res.setHeader('Cross-Origin-Resource-Policy', 'cross-origin');
      next();
    });
  },
};

export default defineConfig({
  plugins: [crossOriginIsolation],
  worker: { format: 'es' },
  esbuild: { target: 'esnext' },
  build: {
    target: 'esnext',
    sourcemap: false,
    assetsInlineLimit: 0,
    rollupOptions: { output: { manualChunks: undefined } },
  },
  server: {
    host: '0.0.0.0',
    port: 5173,
    strictPort: true,
    // Aceita qualquer host de preview (proxy da plataforma) — o app é servido
    // em domínios dinâmicos tipo 5173-xxxx.e2b.app.
    allowedHosts: true,
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
    fs: { strict: false },
  },
  preview: {
    host: '0.0.0.0',
    port: 4173,
    allowedHosts: true,
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
});
