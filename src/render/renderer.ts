/**
 * ============================================================================
 *  RENDERER WEBGPU
 * ============================================================================
 *  Princípios de performance deste arquivo:
 *   • 4-5 DRAW CALLS POR CHUNK, no máximo 2 por segmento (opaco/translúcido),
 *     com `setVertexBuffer(buf, offset, size)` — sem rebind de pipelines,
 *     sem bind group por chunk (o atlas é global, o uniform é global).
 *   • Vértice de 16 B (u32 packed) → 30-50% menos banda de memória.
 *   • Frustum culling por AABB em JS puro (zero alocação por frame).
 *   • Upload de mesh em 2 ranges exatos (opaco + translúcido), direto da
 *     memória compartilhada (writeBuffer lê do SAB sem cópia intermediária).
 *   • MSAA 4× opcional com resolve na swapchain (bordas limpas sem postFX).
 *   • Instanced draw para corpos rígidos: 1 draw call para até 4096 cubos.
 */

import { LAYOUT, MAX_BODIES, MESH_HDR_BYTES, VERTEX_BYTES } from '../core/memory';
import { createAtlasTexture } from './atlas';
import { CHUNK_WGSL, SKY_WGSL, BODIES_WGSL, OUTLINE_WGSL } from './shaders';
import { mat4Perspective, mat4LookAt, mat4Mul, extractFrustum, aabbInFrustum } from '../core/math';

export interface CameraState {
  position: Float32Array;      // [x,y,z]
  yaw: number; pitch: number;  // radianos
  fovDeg: number;
  frustum: Float32Array;       // 6 planos (preenchido por updateCamera)
  maxDist: number;             // corte por distância (blocos)
}

export interface FrameStats {
  drawCalls: number;
  triangles: number;
  chunksDrawn: number;
  chunksCulled: number;
  instances: number;
  uploads: number;
  gpuMs: number;
}

interface ChunkBuffer {
  buf: GPUBuffer;
  worker: number;
  opaqueVerts: number;
  waterVerts: number;
  /** Deslocamento em bytes do segmento translúcido dentro do buffer (empacotado justo). */
  waterOffset: number;
  min: [number, number, number];
  max: [number, number, number];
  /** distância² ao centro da câmera (para ordenar o blend) */
  sortKey: number;
}

const UNIFORM_BYTES = 128;

export class Renderer {
  readonly device: GPUDevice;
  readonly format: GPUTextureFormat;
  readonly sampleCount: number;
  readonly canvas: HTMLCanvasElement;
  private ctx: GPUCanvasContext;

  private skyPipeline!: GPURenderPipeline;
  private chunkPipeline!: GPURenderPipeline;
  private waterPipeline!: GPURenderPipeline;
  private bodyPipeline!: GPURenderPipeline;
  private outlinePipeline!: GPURenderPipeline;

  private camBuf!: GPUBuffer;
  private skyBuf!: GPUBuffer;
  private instBuf!: GPUBuffer;
  private lineBuf!: GPUBuffer;
  private camBind!: GPUBindGroup;
  private skyBind!: GPUBindGroup;
  private bodyBind!: GPUBindGroup;
  private lineBind!: GPUBindGroup;
  private sampler!: GPUSampler;

  private depthTex: GPUTexture | null = null;
  private msaaTex: GPUTexture | null = null;
  private width = 0; private height = 0;

  /** Um buffer de GPU POR CHUNK (slot do pool), não por worker: o worker é
   *  apenas a "linha de montagem", o destino é sempre o chunk. */
  private chunkBufs: Array<ChunkBuffer | null> = new Array(1024).fill(null);
  /** Lista viva usada no desenho (sem realocação por frame). */
  readonly drawables: ChunkBuffer[] = [];
  private drawList: ChunkBuffer[] = [];
  private waterList: ChunkBuffer[] = [];

  // buffers temporários reutilizados (zero alocação por frame)
  private view = new Float32Array(16);
  private proj = new Float32Array(16);
  private vp = new Float32Array(16);
  private camData = new Float32Array(UNIFORM_BYTES / 4);
  private skyData = new Float32Array(UNIFORM_BYTES / 4);
  private instData: Float32Array;
  private lineData = new Float32Array(24 * 3);
  private zero3 = new Float32Array(3);
  private up = new Float32Array([0, 1, 0]);
  private center = new Float32Array(3);
  private eye = new Float32Array(3);
  private instCount = 0;
  private bodyMax = -1;
  private bodyF32!: Float32Array;
  private bodyI32!: Int32Array;
  readonly stats: FrameStats = { drawCalls: 0, triangles: 0, chunksDrawn: 0, chunksCulled: 0, instances: 0, uploads: 0, gpuMs: 0 };

  constructor(canvas: HTMLCanvasElement, device: GPUDevice, atlasPixels: Uint8Array, sampleCount = 4) {
    this.canvas = canvas;
    this.device = device;
    this.format = navigator.gpu.getPreferredCanvasFormat();
    this.sampleCount = sampleCount;
    this.ctx = canvas.getContext('webgpu') as GPUCanvasContext;
    this.ctx.configure({ device, format: this.format, alphaMode: 'opaque' });
    this.instData = new Float32Array(MAX_BODIES * 8);
    this.buildPipelines(atlasPixels);
  }

  /** Cria device com fallback e limites high-performance. */
  static async createDevice(): Promise<{ device: GPUDevice; adapter: GPUAdapter }> {
    if (!('gpu' in navigator)) throw new Error('WebGPU não está disponível neste navegador.');
    const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!adapter) throw new Error('Nenhum adaptador WebGPU encontrado (GPU bloqueada ou indisponível).');
    // Só pedimos o que realmente precisamos. CUIDADO: `maxVertexBufferArrayStride`
    // é o MAIOR stride permitido — pedir 16 invalidaria o buffer de instâncias
    // (stride 32) e derrubaria a criação do pipeline de corpos rígidos.
    const device = await adapter.requestDevice({
      requiredLimits: {
        maxBufferSize: Math.min(adapter.limits.maxBufferSize, 256 * 1024 * 1024),
      },
    });
    // Erros de validação aparecem no console em vez de falharem silenciosamente.
    device.addEventListener?.('uncapturederror', (ev) => {
      const e = ev as GPUUncapturedErrorEvent;
      console.error('[WebGPU]', e.error.message);
    });
    return { device, adapter };
  }

  // ───────────────────────── Pipelines ─────────────────────────────────────
  private buildPipelines(atlasPixels: Uint8Array): void {
    const d = this.device;
    const atlasTex = createAtlasTexture(d, atlasPixels);
    this.sampler = d.createSampler({
      magFilter: 'nearest', minFilter: 'nearest', mipmapFilter: 'linear',
      addressModeU: 'clamp-to-edge', addressModeV: 'clamp-to-edge',
    });

    const camBgl = d.createBindGroupLayout({
      entries: [
        { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: '2d-array' } },
        { binding: 2, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
      ],
    });
    const skyBgl = d.createBindGroupLayout({
      entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } }],
    });

    this.camBuf = d.createBuffer({ size: UNIFORM_BYTES, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.skyBuf = d.createBuffer({ size: UNIFORM_BYTES, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });

    this.camBind = d.createBindGroup({
      layout: camBgl,
      entries: [
        { binding: 0, resource: { buffer: this.camBuf } },
        { binding: 1, resource: atlasTex.createView({ dimension: '2d-array' }) },
        { binding: 2, resource: this.sampler },
      ],
    });
    this.skyBind = d.createBindGroup({ layout: skyBgl, entries: [{ binding: 0, resource: { buffer: this.skyBuf } }] });
    this.bodyBind = d.createBindGroup({ layout: camBgl, entries: [
      { binding: 0, resource: { buffer: this.camBuf } },
      { binding: 1, resource: atlasTex.createView({ dimension: '2d-array' }) },
      { binding: 2, resource: this.sampler },
    ]});
    this.lineBind = this.bodyBind;

    // ── Pipeline do céu (sem vertex buffer, sem depth write) ──────────────
    const skyMod = d.createShaderModule({ code: SKY_WGSL, label: 'sky' });
    this.skyPipeline = d.createRenderPipeline({
      layout: d.createPipelineLayout({ bindGroupLayouts: [skyBgl] }),
      vertex: { module: skyMod, entryPoint: 'vsSky' },
      fragment: { module: skyMod, entryPoint: 'fsSky', targets: [{ format: this.format }] },
      primitive: { topology: 'triangle-list', cullMode: 'none' },
      depthStencil: { format: 'depth24plus', depthWriteEnabled: false, depthCompare: 'always' },
      multisample: { count: this.sampleCount },
    });

    // ── Pipeline do chunk ────────────────────────────────────────────────
    const chunkMod = d.createShaderModule({ code: CHUNK_WGSL, label: 'chunk' });
    const vertexLayout: GPUVertexBufferLayout = {
      arrayStride: VERTEX_BYTES,
      stepMode: 'vertex',
      attributes: [
        { shaderLocation: 0, offset: 0, format: 'uint32' },
        { shaderLocation: 1, offset: 4, format: 'uint32' },
        { shaderLocation: 2, offset: 8, format: 'uint32' },
        { shaderLocation: 3, offset: 12, format: 'uint32' },
      ],
    };
    const chunkLayout = d.createPipelineLayout({ bindGroupLayouts: [camBgl] });
    this.chunkPipeline = d.createRenderPipeline({
      layout: chunkLayout,
      vertex: { module: chunkMod, entryPoint: 'vsChunk', buffers: [vertexLayout] },
      fragment: { module: chunkMod, entryPoint: 'fsChunk', targets: [{ format: this.format }] },
      primitive: { topology: 'triangle-list', cullMode: 'back', frontFace: 'ccw' },
      depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' },
      multisample: { count: this.sampleCount },
    });
    this.waterPipeline = d.createRenderPipeline({
      layout: chunkLayout,
      vertex: { module: chunkMod, entryPoint: 'vsChunk', buffers: [vertexLayout] },
      fragment: {
        module: chunkMod, entryPoint: 'fsChunk',
        targets: [{
          format: this.format,
          blend: {
            color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha', operation: 'add' },
            alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' },
          },
        }],
      },
      primitive: { topology: 'triangle-list', cullMode: 'none' },   // água vista de dentro
      depthStencil: { format: 'depth24plus', depthWriteEnabled: false, depthCompare: 'less' },
      multisample: { count: this.sampleCount },
    });

    // ── Pipeline dos corpos rígidos (instanced) ──────────────────────────
    const bodyMod = d.createShaderModule({ code: BODIES_WGSL, label: 'bodies' });
    this.instBuf = d.createBuffer({ size: MAX_BODIES * 32, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    const cube = buildCubeGeometry();
    const cubeBuf = d.createBuffer({ size: cube.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    d.queue.writeBuffer(cubeBuf, 0, cube as unknown as GPUAllowSharedBufferSource);
    this.bodyPipeline = d.createRenderPipeline({
      layout: d.createPipelineLayout({ bindGroupLayouts: [camBgl] }),
      vertex: {
        module: bodyMod, entryPoint: 'vsBody',
        buffers: [
          { arrayStride: 24, attributes: [
            { shaderLocation: 0, offset: 0, format: 'float32x3' },
            { shaderLocation: 1, offset: 12, format: 'float32x3' },
          ]},
          { arrayStride: 32, stepMode: 'instance', attributes: [
            { shaderLocation: 2, offset: 0, format: 'float32x3' },
            { shaderLocation: 3, offset: 12, format: 'float32' },
            { shaderLocation: 4, offset: 16, format: 'float32x4' },
          ]},
        ],
      },
      fragment: { module: bodyMod, entryPoint: 'fsBody', targets: [{ format: this.format }] },
      primitive: { topology: 'triangle-list', cullMode: 'back' },
      depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' },
      multisample: { count: this.sampleCount },
    });
    this.cubeBuf = cubeBuf;
    this.cubeVerts = cube.length / 24;

    // ── Pipeline do contorno de seleção (wireframe) ──────────────────────
    const lineMod = d.createShaderModule({ code: OUTLINE_WGSL, label: 'outline' });
    this.lineBuf = d.createBuffer({ size: this.lineData.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
    this.outlinePipeline = d.createRenderPipeline({
      layout: d.createPipelineLayout({ bindGroupLayouts: [camBgl] }),
      vertex: { module: lineMod, entryPoint: 'vsLine', buffers: [{ arrayStride: 12, attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x3' }] }] },
      fragment: { module: lineMod, entryPoint: 'fsLine', targets: [{ format: this.format, blend: {
        color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha', operation: 'add' },
        alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' },
      } }] },
      primitive: { topology: 'line-list' },
      depthStencil: { format: 'depth24plus', depthWriteEnabled: false, depthCompare: 'less-equal' },
      multisample: { count: this.sampleCount },
    });
  }

  private cubeBuf!: GPUBuffer;
  private cubeVerts = 36;

  // ─────────────────── Buffers de chunk / upload ───────────────────────────
  /**
   * Buffer de GPU POR CHUNK, dimensionado pelo mesh real (não pelo rascunho).
   * Cresce com histerese de 25% para nunca realocar a cada remesh — o consumo
   * típico fica em ~80-160 KB por chunk (60-90 MB para 576 chunks).
   */
  private ensureChunkBuffer(slot: number, neededBytes: number): ChunkBuffer {
    let cb = this.chunkBufs[slot];
    const want = Math.max(4096, (neededBytes * 1.25) | 0);
    if (cb && cb.buf.size >= want) return cb;
    if (!cb) {
      cb = { buf: null as unknown as GPUBuffer, worker: -1, opaqueVerts: 0, waterVerts: 0, waterOffset: 0,
             min: [0, 0, 0], max: [0, 0, 0], sortKey: 0 };
      this.chunkBufs[slot] = cb;
      this.drawables.push(cb);
    } else {
      cb.buf.destroy();
    }
    cb.buf = this.device.createBuffer({
      size: want,
      usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
      label: `chunk-${slot}`,
    });
    return cb;
  }

  /**
   * Sobe um mesh pronto direto da memória compartilhada para a VRAM.
   * 2 transferências exatas (opaco e translúcido) — nada de copiar 640 KB.
   */
  uploadMesh(sab: SharedArrayBuffer, worker: number, chunkSlot: number): void {
    const base = LAYOUT.meshOut + worker * LAYOUT.meshSlotBytes;
    const head = new Uint32Array(sab, base, 8);
    const fhead = new Float32Array(sab, base + 16, 6);
    const opaqueVerts = head[0];
    const waterVerts = head[4];
    const sectionCap = LAYOUT.meshSectionCap;
    const opaqueBytes = opaqueVerts * VERTEX_BYTES;
    const waterBytes = waterVerts * VERTEX_BYTES;

    // Empacotamento justo na VRAM: [opacos][translúcidos] — sem padding.
    const cb = this.ensureChunkBuffer(chunkSlot, opaqueBytes + waterBytes);
    cb.worker = worker;
    cb.waterOffset = opaqueBytes;

    if (opaqueBytes > 0) {
      this.pushFromSAB(cb.buf, 0, sab, base + MESH_HDR_BYTES, opaqueBytes);
    }
    if (waterBytes > 0) {
      // A origem está após o segmento opaco na memória de rascunho (256 KB fixos).
      this.pushFromSAB(cb.buf, opaqueBytes, sab, base + MESH_HDR_BYTES + sectionCap, waterBytes);
    }
    cb.opaqueVerts = opaqueVerts;
    cb.waterVerts = waterVerts;
    if (opaqueVerts + waterVerts > 0) {
      cb.min = [fhead[0], fhead[1], fhead[2]];
      cb.max = [fhead[3], fhead[4], fhead[5]];
    } else {
      cb.min = [0, 0, 0]; cb.max = [0, 0, 0];
    }
    this.stats.uploads++;
  }

  // ────────────────────────────── Câmera ───────────────────────────────────
  /** Atualiza matrizes + frustum (zero alocação). */
  updateCamera(cam: CameraState, timeSec: number, fogColor: [number, number, number], fogDensity: number,
               sunDir: [number, number, number], ambient: number, dayFactor: number): void {
    const cp = Math.cos(cam.pitch), sp = Math.sin(cam.pitch);
    const cy = Math.cos(cam.yaw), sy = Math.sin(cam.yaw);
    // Frente (yaw=0 → -Z), direita e cima — a mesma base usada pelo céu.
    const fx = -sy * cp, fy = sp, fz = -cy * cp;
    this.eye[0] = cam.position[0]; this.eye[1] = cam.position[1]; this.eye[2] = cam.position[2];
    this.center[0] = this.eye[0] + fx; this.center[1] = this.eye[1] + fy; this.center[2] = this.eye[2] + fz;

    const aspect = this.canvas.width / Math.max(1, this.canvas.height);
    mat4LookAt(this.view, this.eye as unknown as [number, number, number], this.center as unknown as [number, number, number],
               this.up as unknown as [number, number, number]);
    mat4Perspective(this.proj, (cam.fovDeg * Math.PI) / 180, aspect, 0.1, 1200);
    mat4Mul(this.vp, this.proj, this.view);
    extractFrustum(this.vp, cam.frustum);

    // Uniform da câmera: matriz + posição/tempo + fog + sol + params
    this.camData.set(this.vp, 0);
    this.camData[16] = this.eye[0]; this.camData[17] = this.eye[1]; this.camData[18] = this.eye[2]; this.camData[19] = timeSec;
    this.camData[20] = fogColor[0]; this.camData[21] = fogColor[1]; this.camData[22] = fogColor[2]; this.camData[23] = fogDensity;
    this.camData[24] = sunDir[0]; this.camData[25] = sunDir[1]; this.camData[26] = sunDir[2]; this.camData[27] = ambient;
    this.camData[28] = 0; this.camData[29] = 0.06; this.camData[30] = 0; this.camData[31] = 0;
    this.device.queue.writeBuffer(this.camBuf, 0, this.camData as unknown as GPUAllowSharedBufferSource);

    // Uniform do céu
    const right = [cy, 0, -sy];                 // base da câmera
    const upv = [-sy * sp, cp, -cy * sp];
    this.skyData[0] = fx; this.skyData[1] = fy; this.skyData[2] = fz; this.skyData[3] = Math.tan((cam.fovDeg * Math.PI) / 360);
    this.skyData[4] = right[0]; this.skyData[5] = right[1]; this.skyData[6] = right[2]; this.skyData[7] = aspect;
    this.skyData[8] = upv[0]; this.skyData[9] = upv[1]; this.skyData[10] = upv[2]; this.skyData[11] = 0;
    this.skyData[12] = sunDir[0]; this.skyData[13] = sunDir[1]; this.skyData[14] = sunDir[2]; this.skyData[15] = dayFactor;
    // Céu diurno ↔ noturno (interpolação contínua, sem "pop")
    const day = Math.max(0, Math.min(1, dayFactor));
    this.skyData[16] = 0.16 + 0.14 * day; this.skyData[17] = 0.38 + 0.22 * day; this.skyData[18] = 0.86 - 0.14 * day; this.skyData[19] = 1;
    this.skyData[20] = fogColor[0]; this.skyData[21] = fogColor[1]; this.skyData[22] = fogColor[2]; this.skyData[23] = 1;
    this.skyData[24] = fogColor[0] * 0.55; this.skyData[25] = fogColor[1] * 0.5; this.skyData[26] = fogColor[2] * 0.45; this.skyData[27] = 1;
    this.skyData[28] = this.canvas.width; this.skyData[29] = this.canvas.height;
    this.device.queue.writeBuffer(this.skyBuf, 0, this.skyData as unknown as GPUAllowSharedBufferSource);
  }

  /** Corpos rígidos: preenche o instance buffer direto do SAB (1 draw call). */
  updateBodies(sab: SharedArrayBuffer, maxBodies: number): void {
    // Views criadas uma única vez por tamanho (nunca por frame).
    if (this.bodyMax !== maxBodies) {
      this.bodyMax = maxBodies;
      this.bodyF32 = new Float32Array(sab, LAYOUT.bodies, maxBodies * 16);
      this.bodyI32 = new Int32Array(sab, LAYOUT.bodies, maxBodies * 16);
    }
    const src = this.bodyF32;
    const si = this.bodyI32;
    let n = 0;
    for (let i = 0; i < maxBodies; i++) {
      const o = i * 16;
      if (si[o + 7] === 0) continue;                  // FLAG_ACTIVE
      const d = n * 8;
      this.instData[d] = src[o]; this.instData[d + 1] = src[o + 1]; this.instData[d + 2] = src[o + 2];
      this.instData[d + 3] = src[o + 6];              // meio-tamanho
      const col = si[o + 8] >>> 0;
      this.instData[d + 4] = ((col >>> 24) & 255) / 255;
      this.instData[d + 5] = ((col >>> 16) & 255) / 255;
      this.instData[d + 6] = ((col >>> 8) & 255) / 255;
      this.instData[d + 7] = 1;
      n++;
    }
    this.instCount = n;
    if (n > 0) this.device.queue.writeBuffer(this.instBuf, 0, this.instData as unknown as GPUAllowSharedBufferSource, 0, n * 8);
  }

  /** Contorno do bloco selecionado — 12 arestas, 1 draw call. */
  setSelectionOutline(x: number, y: number, z: number, visible: boolean): void {
    if (!visible) { this.lineVerts = 0; return; }
    const e = 0.003;
    const a: [number, number, number] = [x - e, y - e, z - e];
    const b: [number, number, number] = [x + 1 + e, y + 1 + e, z + 1 + e];
    const c: Array<[number, number, number]> = [
      [a[0], a[1], a[2]], [b[0], a[1], a[2]], [a[0], a[1], b[2]], [b[0], a[1], b[2]],
      [a[0], b[1], a[2]], [b[0], b[1], a[2]], [a[0], b[1], b[2]], [b[0], b[1], b[2]],
    ];
    const edges = [0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 1, 3, 4, 6, 5, 7, 0, 4, 1, 5, 2, 6, 3, 7];
    let o = 0;
    for (const idx of edges) {
      this.lineData[o++] = c[idx][0]; this.lineData[o++] = c[idx][1]; this.lineData[o++] = c[idx][2];
    }
    this.device.queue.writeBuffer(this.lineBuf, 0, this.lineData);
    this.lineVerts = 24;
  }
  private lineVerts = 0;

  // ─────────────────────────────── Resize ──────────────────────────────────
  resize(cssWidth: number, cssHeight: number, scale: number, dpr: number): boolean {
    const w = Math.max(2, Math.floor(cssWidth * dpr * scale));
    const h = Math.max(2, Math.floor(cssHeight * dpr * scale));
    if (w === this.width && h === this.height) return false;
    this.width = w; this.height = h;
    this.canvas.width = w; this.canvas.height = h;

    this.depthTex?.destroy();
    this.msaaTex?.destroy();
    this.depthTex = this.device.createTexture({
      size: [w, h], format: 'depth24plus', sampleCount: this.sampleCount,
      usage: GPUTextureUsage.RENDER_ATTACHMENT, label: 'depth',
    });
    if (this.sampleCount > 1) {
      this.msaaTex = this.device.createTexture({
        size: [w, h], format: this.format, sampleCount: this.sampleCount,
        usage: GPUTextureUsage.RENDER_ATTACHMENT, label: 'msaa-color',
      });
    } else this.msaaTex = null;
    return true;
  }

  // ──────────────────────────────── Frame ──────────────────────────────────
  /** Desenha um frame completo. Custo de CPU esperado: < 1,5 ms. */
  render(cam: CameraState, chunks: ChunkBuffer[], onlyLit = true): void {
    void onlyLit;
    const s = this.stats;
    s.drawCalls = 0; s.triangles = 0; s.chunksDrawn = 0; s.chunksCulled = 0; s.instances = this.instCount;

    const cx = cam.position[0], cy = cam.position[1], cz = cam.position[2];
    this.drawList.length = 0;
    this.waterList.length = 0;

    // ── Frustum culling + ordenação por distância (blend correto) ─────────
    for (let i = 0; i < chunks.length; i++) {
      const c = chunks[i];
      if (c.opaqueVerts + c.waterVerts === 0) continue;
      const dx = (c.min[0] + c.max[0]) / 2 - cx;
      const dy = (c.min[1] + c.max[1]) / 2 - cy;
      const dz = (c.min[2] + c.max[2]) / 2 - cz;
      c.sortKey = dx * dx + dy * dy + dz * dz;
      const maxD = cam.maxDist || 1e9;
      if (c.sortKey > maxD * maxD) { s.chunksCulled++; continue; }
      if (!aabbInFrustum(cam.frustum, c.min[0], c.min[1], c.min[2], c.max[0], c.max[1], c.max[2])) {
        s.chunksCulled++;
        continue;
      }
      this.drawList.push(c);
      if (c.waterVerts > 0) this.waterList.push(c);
    }
    this.drawList.sort((a, b) => a.sortKey - b.sortKey);              // frente → trás
    this.waterList.sort((a, b) => b.sortKey - a.sortKey);             // trás → frente

    const colorView = this.sampleCount > 1 ? this.msaaTex!.createView() : this.ctx.getCurrentTexture().createView();
    const resolveView = this.sampleCount > 1 ? this.ctx.getCurrentTexture().createView() : undefined;

    const enc = this.device.createCommandEncoder();
    const pass = enc.beginRenderPass({
      colorAttachments: [{
        view: colorView,
        resolveTarget: resolveView,
        clearValue: { r: 0.05, g: 0.07, b: 0.11, a: 1 },
        loadOp: 'clear',
        storeOp: this.sampleCount > 1 ? 'discard' : 'store',
      }],
      depthStencilAttachment: {
        view: this.depthTex!.createView(),
        depthClearValue: 1.0,
        depthLoadOp: 'clear',
        depthStoreOp: 'discard',
      },
    });

    // 1) Céu (1 draw call, triângulo gigante)
    pass.setPipeline(this.skyPipeline);
    pass.setBindGroup(0, this.skyBind);
    pass.draw(3);
    s.drawCalls++;

    // 2) Chunks opacos (frente → trás = early-z perfeito)
    pass.setPipeline(this.chunkPipeline);
    pass.setBindGroup(0, this.camBind);
    for (let i = 0; i < this.drawList.length; i++) {
      const c = this.drawList[i];
      if (c.opaqueVerts === 0) continue;
      pass.setVertexBuffer(0, c.buf, 0, c.opaqueVerts * VERTEX_BYTES);
      pass.draw(c.opaqueVerts);
      s.drawCalls++; s.triangles += c.opaqueVerts / 3; s.chunksDrawn++;
    }

    // 3) Corpos rígidos (instanced, dentro do passe opaco)
    if (this.instCount > 0) {
      pass.setPipeline(this.bodyPipeline);
      pass.setBindGroup(0, this.bodyBind);
      pass.setVertexBuffer(0, this.cubeBuf);
      pass.setVertexBuffer(1, this.instBuf, 0, this.instCount * 32);
      pass.draw(this.cubeVerts, this.instCount);
      s.drawCalls++; s.triangles += (this.cubeVerts / 3) * this.instCount;
    }

    // 4) Translúcidos (trás → frente, sem escrever profundidade)
    if (this.waterList.length) {
      pass.setPipeline(this.waterPipeline);
      pass.setBindGroup(0, this.camBind);
      for (let i = 0; i < this.waterList.length; i++) {
        const c = this.waterList[i];
        pass.setVertexBuffer(0, c.buf, c.waterOffset, c.waterVerts * VERTEX_BYTES);
        pass.draw(c.waterVerts);
        s.drawCalls++; s.triangles += c.waterVerts / 3;
      }
    }

    // 5) Contorno de seleção
    if (this.lineVerts > 0) {
      pass.setPipeline(this.outlinePipeline);
      pass.setBindGroup(0, this.lineBind);
      pass.setVertexBuffer(0, this.lineBuf, 0, this.lineVerts * 12);
      pass.draw(this.lineVerts);
      s.drawCalls++;
    }

    pass.end();
    this.device.queue.submit([enc.finish()]);
  }

  /**
   * Upload direto da memória compartilhada (sem cópia) — com plano B.
   * A especificação marca a fonte como [AllowShared] BufferSource, mas drivers
   * antigos recusam SAB: nesse caso copiamos por um staging buffer. Custo do
   * plano B: um memcpy de ~80 KB só em GPUs antigas.
   */
  private staging = new Uint8Array(LAYOUT.meshSlotBytes);
  private pushFromSAB(dst: GPUBuffer, dstOffset: number, sab: SharedArrayBuffer, srcOffset: number, size: number): void {
    try {
      this.device.queue.writeBuffer(dst, dstOffset, sab as unknown as GPUAllowSharedBufferSource, srcOffset, size);
    } catch {
      const view = new Uint8Array(sab, srcOffset, size);
      this.staging.set(view, 0);
      this.device.queue.writeBuffer(dst, dstOffset, this.staging.buffer as ArrayBuffer, 0, size);
    }
  }

  /** Acesso ao buffer de GPU de um worker (usado pela engine no render). */
  chunkSlotBuffer(worker: number): ChunkBuffer | null { return this.chunkBufs[worker]; }

  /** Libera buffers de um worker (ex.: ao reduzir a qualidade). */
  disposeWorker(worker: number): void { this.chunkBufs[worker]?.buf.destroy(); this.chunkBufs[worker] = undefined as unknown as ChunkBuffer; }
}

/** Cubo unitário (36 vértices, 24 B: posição + normal) — base dos corpos. */
function buildCubeGeometry(): Float32Array {
  const faces: Array<[number[], number[][]]> = [
    [[1, 0, 0], [[0.5, -0.5, -0.5], [0.5, 0.5, -0.5], [0.5, 0.5, 0.5], [0.5, -0.5, 0.5]]],
    [[-1, 0, 0], [[-0.5, -0.5, 0.5], [-0.5, 0.5, 0.5], [-0.5, 0.5, -0.5], [-0.5, -0.5, -0.5]]],
    [[0, 1, 0], [[-0.5, 0.5, -0.5], [-0.5, 0.5, 0.5], [0.5, 0.5, 0.5], [0.5, 0.5, -0.5]]],
    [[0, -1, 0], [[-0.5, -0.5, 0.5], [-0.5, -0.5, -0.5], [0.5, -0.5, -0.5], [0.5, -0.5, 0.5]]],
    [[0, 0, 1], [[-0.5, -0.5, 0.5], [0.5, -0.5, 0.5], [0.5, 0.5, 0.5], [-0.5, 0.5, 0.5]]],
    [[0, 0, -1], [[0.5, -0.5, -0.5], [-0.5, -0.5, -0.5], [-0.5, 0.5, -0.5], [0.5, 0.5, -0.5]]],
  ];
  const out = new Float32Array(36 * 6);
  let o = 0;
  for (const [n, v] of faces) {
    const idx = [0, 1, 2, 0, 2, 3];
    for (const k of idx) {
      out[o++] = v[k][0]; out[o++] = v[k][1]; out[o++] = v[k][2];
      out[o++] = n[0]; out[o++] = n[1]; out[o++] = n[2];
    }
  }
  return out;
}

export type { ChunkBuffer };
export { aabbInFrustum };
