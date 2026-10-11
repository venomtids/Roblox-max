/**
 * ============================================================================
 *  MATEMÁTICA 3D — vetores, matrizes 4×4 e frustum culling
 * ============================================================================
 *  Tudo em Float32Array "out-first" (sem alocação por frame, zero GC).
 *  Convenções:
 *   • Coluna-maior (padrão WebGPU/WGSL): m[col*4 + row].
 *   • Mundo right-handed; clip space da WebGPU com profundidade em [0,1].
 *   • Planos de frustum extraídos direto da view-projection (custo ~0).
 */

export type Vec3 = [number, number, number];

export function mat4Identity(out: Float32Array): Float32Array {
  out.fill(0); out[0] = out[5] = out[10] = out[15] = 1; return out;
}

/** Multiplicação out = a * b (column-major). */
export function mat4Mul(out: Float32Array, a: Float32Array, b: Float32Array): Float32Array {
  for (let c = 0; c < 4; c++) {
    const b0 = b[c * 4], b1 = b[c * 4 + 1], b2 = b[c * 4 + 2], b3 = b[c * 4 + 3];
    out[c * 4]     = a[0] * b0 + a[4] * b1 + a[8]  * b2 + a[12] * b3;
    out[c * 4 + 1] = a[1] * b0 + a[5] * b1 + a[9]  * b2 + a[13] * b3;
    out[c * 4 + 2] = a[2] * b0 + a[6] * b1 + a[10] * b2 + a[14] * b3;
    out[c * 4 + 3] = a[3] * b0 + a[7] * b1 + a[11] * b2 + a[15] * b3;
  }
  return out;
}

/** Projeção perspectiva com depth [0,1] (WebGPU, não OpenGL). */
export function mat4Perspective(out: Float32Array, fovY: number, aspect: number, near: number, far: number): Float32Array {
  const f = 1 / Math.tan(fovY / 2);
  out.fill(0);
  out[0] = f / aspect;
  out[5] = f;
  out[11] = -1;
  out[10] = far / (near - far);           // depth 0..1
  out[14] = (far * near) / (near - far);
  return out;
}

/** View matrix right-handed (câmera olhando para -Z). */
export function mat4LookAt(out: Float32Array, eye: Vec3, center: Vec3, up: Vec3): Float32Array {
  let zx = eye[0] - center[0], zy = eye[1] - center[1], zz = eye[2] - center[2];
  let zl = Math.hypot(zx, zy, zz) || 1; zx /= zl; zy /= zl; zz /= zl;
  let xx = up[1] * zz - up[2] * zy, xy = up[2] * zx - up[0] * zz, xz = up[0] * zy - up[1] * zx;
  let xl = Math.hypot(xx, xy, xz) || 1; xx /= xl; xy /= xl; xz /= xl;
  const yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
  out[0] = xx; out[1] = yx; out[2] = zx; out[3] = 0;
  out[4] = xy; out[5] = yy; out[6] = zy; out[7] = 0;
  out[8] = xz; out[9] = yz; out[10] = zz; out[11] = 0;
  out[12] = -(xx * eye[0] + xy * eye[1] + xz * eye[2]);
  out[13] = -(yx * eye[0] + yy * eye[1] + yz * eye[2]);
  out[14] = -(zx * eye[0] + zy * eye[1] + zz * eye[2]);
  out[15] = 1;
  return out;
}

/** Extrai 6 planos (a,b,c,d) normalizados da view-projection → out[24]. */
export function extractFrustum(vp: Float32Array, out: Float32Array): void {
  const r = (i: number) => [vp[i], vp[4 + i], vp[8 + i], vp[12 + i]];
  const [l0, l1, l2, l3] = r(0);
  const [u0, u1, u2, u3] = r(1);
  const [f0, f1, f2, f3] = r(2);
  const [w0, w1, w2, w3] = r(3);
  const planes = [
    w0 + l0, w1 + l1, w2 + l2, w3 + l3,   // esquerda
    w0 - l0, w1 - l1, w2 - l2, w3 - l3,   // direita
    w0 + u0, w1 + u1, w2 + u2, w3 + u3,   // baixo
    w0 - u0, w1 - u1, w2 - u2, w3 - u3,   // cima
    f0, f1, f2, f3,                        // near (z=0 no NDC da WebGPU)
    w0 - f0, w1 - f1, w2 - f2, w3 - f3,   // far  (z=1)
  ];
  for (let i = 0; i < 6; i++) {
    const a = planes[i * 4], b = planes[i * 4 + 1], c = planes[i * 4 + 2], d = planes[i * 4 + 3];
    const len = Math.hypot(a, b, c) || 1;
    out[i * 4] = a / len; out[i * 4 + 1] = b / len; out[i * 4 + 2] = c / len; out[i * 4 + 3] = d / len;
  }
}

/** Teste AABB × frustum pelo "vértice positivo" (o mais rápido e sem sqrt). */
export function aabbInFrustum(p: Float32Array, x0: number, y0: number, z0: number, x1: number, y1: number, z1: number): boolean {
  for (let i = 0; i < 6; i++) {
    const a = p[i * 4], b = p[i * 4 + 1], c = p[i * 4 + 2], d = p[i * 4 + 3];
    const px = a >= 0 ? x1 : x0, py = b >= 0 ? y1 : y0, pz = c >= 0 ? z1 : z0;
    if (a * px + b * py + c * pz + d < 0) return false;
  }
  return true;
}

export const clamp = (v: number, a: number, b: number) => (v < a ? a : v > b ? b : v);
export const lerp = (a: number, b: number, t: number) => a + (b - a) * t;
