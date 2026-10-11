/**
 * ============================================================================
 *  DIAGNÓSTICO DO NAVEGADOR + PERFIL DE QUALIDADE AUTOMÁTICO
 * ============================================================================
 *  O navegador do usuário é o "hardware alvo" mais heterogêneo que existe.
 *  Este módulo responde duas perguntas antes de inicializar a engine:
 *
 *   1) O QUE ESTÁ FALTANDO? (WebGPU, aceleração de hardware, SAB, SIMD)
 *      → e a instrução EXATA de como habilitar, por navegador.
 *   2) COM QUE QUALIDADE COMEÇAR? (MSAA, escala de render, distância, peças)
 *      → perfil derivado de núcleos, memória, tipo de dispositivo e navegador.
 *
 *  Casos reais que isto resolve:
 *   • Brave com Shields agressivos: WebGPU some (anti-fingerprint) → instruções.
 *   • Opera/GX com aceleração desligada: cai em SwiftShader/software → instruções.
 *   • Notebook com GPU integrada: 4× MSAA a 100% de escala entrega 25 FPS →
 *     o perfil baixa para 2× e 0.85 e o jogo fica em 60.
 */

export type BrowserId = 'brave' | 'opera-gx' | 'opera' | 'edge' | 'chrome' | 'firefox' | 'safari' | 'outro';

export interface DiagnosisItem {
  label: string;
  ok: boolean;
  critical: boolean;
  detail: string;
  fix?: string;
}

export interface BrowserReport {
  browser: BrowserId;
  browserName: string;
  chromium: boolean;
  webgpu: boolean;
  hardwareAccel: boolean | null;
  isolated: boolean;
  sab: boolean;
  cores: number;
  deviceMemoryGB: number | null;
  mobile: boolean;
  items: DiagnosisItem[];
  /** Faltando algo crítico → não vale tentar inicializar. */
  blocked: boolean;
  summary: string;
}

export interface QualityProfile {
  msaa: 1 | 2 | 4;
  renderScale: number;
  viewDistance: number;
  fov: number;
  bodyCount: number;
  daySpeed: number;
  tier: 'alto' | 'medio' | 'leve' | 'celular';
}

const CONF = {
  brave: {
    name: 'Brave',
    accelPath: 'brave://settings/system → "Use hardware acceleration when available"',
    flagsPath: 'brave://flags/#enable-unsafe-webgpu → Enabled',
    gpuPath: 'brave://gpu (procure "WebGPU: Hardware accelerated")',
    extra: 'Shields neste site → Standard (o modo Agressivo bloqueia WebGL/WebGPU por anti-fingerprint)',
  },
  opera: {
    name: 'Opera / Opera GX',
    accelPath: 'opera://settings/system → "Use hardware acceleration when available" LIGADO',
    flagsPath: 'opera://flags/#enable-unsafe-webgpu → Enabled',
    gpuPath: 'opera://gpu (precisa aparecer uma GPU; se aparecer "SwiftShader" é software)',
    extra: 'No Opera GX, desative o "Limiter" para esta aba (ele limita CPU/RAM do navegador)',
  },
  chrome: {
    name: 'Chrome',
    accelPath: 'chrome://settings/system → "Use hardware acceleration when available"',
    flagsPath: 'chrome://flags/#enable-unsafe-webgpu → Enabled (só necessário em versões antigas)',
    gpuPath: 'chrome://gpu',
    extra: '',
  },
  edge: {
    name: 'Edge',
    accelPath: 'edge://settings/system → "Use hardware acceleration when available"',
    flagsPath: 'edge://flags/#enable-unsafe-webgpu → Enabled',
    gpuPath: 'edge://gpu',
    extra: '',
  },
  firefox: {
    name: 'Firefox',
    accelPath: 'about:preferences → Geral → Desempenho → usar aceleração de hardware',
    flagsPath: 'about:config → dom.webgpu.enabled = true (e gfx.webgpu.force-enabled = true)',
    gpuPath: 'about:support (procure "WebGPU")',
    extra: 'Firefox precisa da versão mais recente; o suporte a WebGPU é mais novo que no Chromium',
  },
  safari: {
    name: 'Safari',
    accelPath: 'Safari já usa a GPU por padrão em WebGPU',
    flagsPath: 'Ajustes → Avançado → "Mostrar menu Desenvolver" → Develop → Feature Flags → WebGPU',
    gpuPath: 'Não aplicável',
    extra: 'Requer Safari 18+ (macOS 15+) para WebGPU estável',
  },
  outro: {
    name: 'Navegador não identificado',
    accelPath: 'Configurações do navegador → Sistema → aceleração de hardware',
    flagsPath: 'Procure por "WebGPU" nas flags internas do navegador',
    gpuPath: 'Página interna de GPU do navegador',
    extra: '',
  },
} as const;

/** Detecta o navegador pelo UA + APIs proprietárias (Brave se esconde no UA). */
export function detectBrowser(): { id: BrowserId; chromium: boolean } {
  const nav = navigator as Navigator & { brave?: unknown; userAgentData?: { brands?: Array<{ brand: string }> } };
  const ua = navigator.userAgent;
  const brands = (nav.userAgentData?.brands ?? []).map((b) => b.brand).join(' ');
  if (nav.brave !== undefined || /Brave/i.test(brands)) return { id: 'brave', chromium: true };
  if (/OPR\/|Opera/i.test(ua) || /Opera/i.test(brands)) {
    return { id: /GX/i.test(ua) ? 'opera-gx' : 'opera', chromium: true };
  }
  if (/Edg\//.test(ua)) return { id: 'edge', chromium: true };
  if (/Firefox\/|FxiOS/.test(ua)) return { id: 'firefox', chromium: false };
  if (/Safari\//.test(ua) && !/Chrome\//.test(ua)) return { id: 'safari', chromium: false };
  if (/Chrome\//.test(ua) || /Chromium/i.test(brands)) return { id: 'chrome', chromium: true };
  return { id: 'outro', chromium: false };
}

/** Heurística de aceleração de hardware (sem WebGPU não há como perguntar direto). */
function guessHardwareAccel(chromium: boolean, webgpu: boolean): boolean | null {
  if (webgpu) return true;
  try {
    const canvas = document.createElement('canvas');
    const gl = canvas.getContext('webgl2') ?? canvas.getContext('webgl');
    if (!gl) return false;
    const dbg = (gl as WebGLRenderingContext).getExtension('WEBGL_debug_renderer_info');
    const renderer = dbg
      ? String((gl as WebGLRenderingContext).getParameter((dbg as { UNMASKED_RENDERER_WEBGL: number }).UNMASKED_RENDERER_WEBGL))
      : '';
    if (/SwiftShader|Software|llvmpipe|Basic Render/i.test(renderer)) return false;
    return renderer ? true : null;
  } catch {
    return null;
  }
  void chromium;
}

/** Relatório completo de compatibilidade, com instruções por navegador. */
export function diagnose(): BrowserReport {
  const { id, chromium } = detectBrowser();
  const conf = CONF[id in CONF ? (id as keyof typeof CONF) : 'outro'];
  const webgpu = typeof navigator !== 'undefined' && 'gpu' in navigator;
  const isolated = (globalThis as unknown as { crossOriginIsolated?: boolean }).crossOriginIsolated === true;
  const sab = typeof SharedArrayBuffer !== 'undefined';
  const cores = navigator.hardwareConcurrency || 4;
  const deviceMemoryGB = (navigator as Navigator & { deviceMemory?: number }).deviceMemory ?? null;
  const mobile = matchMedia('(pointer: coarse)').matches;
  const hardwareAccel = guessHardwareAccel(chromium, webgpu);

  const items: DiagnosisItem[] = [
    {
      label: 'WebGPU (renderização de baixo nível)',
      ok: webgpu,
      critical: true,
      detail: webgpu ? 'API disponível' : 'navigator.gpu ausente',
      fix: webgpu ? undefined : [
        `Aceleração de hardware: ${conf.accelPath}`,
        `Flags: ${conf.flagsPath}`,
        `Confirme em ${conf.gpuPath}`,
        conf.extra,
      ].filter(Boolean).join('  ·  '),
    },
    {
      label: 'Aceleração de hardware da GPU',
      ok: hardwareAccel !== false,
      critical: true,
      detail: hardwareAccel === null ? 'indeterminada (será validada no primeiro frame)' : hardwareAccel ? 'GPU real em uso' : 'renderização por software (SwiftShader)',
      fix: hardwareAccel === false ? `${conf.accelPath}  ·  depois reinicie o navegador por completo` : undefined,
    },
    {
      label: 'SharedArrayBuffer (multithreading)',
      ok: sab && isolated,
      critical: false,
      detail: sab && isolated ? `${Math.max(1, cores - 1)} núcleos dedicados ao mundo` : 'indisponível — geração na main thread',
      fix: sab && !isolated
        ? 'A página precisa ser servida com COOP+COEP (o dev server já envia). Servida por file:// ou iframe de terceiros, o multithreading é desligado automaticamente.'
        : undefined,
    },
    {
      label: 'WASM SIMD',
      ok: true,   // validado de verdade pelo núcleo na inicialização
      critical: false,
      detail: 'verificado pelo binário do núcleo',
    },
    {
      label: 'Núcleos lógicos de CPU',
      ok: cores >= 2,
      critical: false,
      detail: `${cores} núcleos${deviceMemoryGB ? ` · ~${deviceMemoryGB} GB de RAM` : ''}`,
    },
  ];

  const blocked = !webgpu;
  const browserName = conf.name;
  const summary = blocked
    ? `WebGPU indisponível no ${browserName}. Siga as instruções abaixo — o jogo não inicia sem ele.`
    : `Compatível. ${browserName}, ${cores} núcleos${sab && isolated ? ', multithreading ativo' : ', modo single-thread'}.`;

  return { browser: id, browserName, chromium, webgpu, hardwareAccel, isolated, sab, cores, deviceMemoryGB, mobile, items, blocked, summary };
}

/** Perfil de qualidade derivado da máquina (o usuário pode mudar no menu ESC). */
export function suggestQuality(report: BrowserReport): QualityProfile {
  const { cores, deviceMemoryGB, mobile, hardwareAccel } = report;
  const weakGpu = hardwareAccel === false;
  const ram = deviceMemoryGB ?? 8;

  if (mobile) {
    return { msaa: 1, renderScale: 0.7, viewDistance: 160, fov: 82, bodyCount: 300, daySpeed: 1 / 300, tier: 'celular' };
  }
  if (weakGpu || cores <= 2 || ram <= 4) {
    return { msaa: 1, renderScale: 0.6, viewDistance: 128, fov: 78, bodyCount: 250, daySpeed: 1 / 360, tier: 'leve' };
  }
  if (cores <= 6 || ram <= 8) {
    return { msaa: 2, renderScale: 0.85, viewDistance: 224, fov: 78, bodyCount: 600, daySpeed: 1 / 240, tier: 'medio' };
  }
  return { msaa: 4, renderScale: 1, viewDistance: 320, fov: 78, bodyCount: 900, daySpeed: 1 / 240, tier: 'alto' };
}
