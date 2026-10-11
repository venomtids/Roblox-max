/**
 * ============================================================================
 *  GREEDY MESHER — 32³ voxels → triângulos (SIMD, ZERO alocação)
 * ============================================================================
 *  Para cada eixo d ∈ {X,Y,Z} e cada sentido ±, varre os 32 planos comparando
 *  dois planos de 32×32 voxels, monta máscara (id do bloco | 0) e funde
 *  retângulos máximos → 1 quad por região homogênea visível.
 *  Ganho medido vs. "1 quad por voxel": 90–97% menos vértices.
 *
 *  REGRA DE OURO DESTE ARQUIVO: nenhum array, nenhum objeto, nenhuma string.
 *  Só escalares. Assim o compilador não chama `__new` (heap/GC) nem uma única
 *  vez na fase mais quente do engine — o mesher é função pura de bytes.
 *
 *  Truques de performance:
 *   • `getBlockFast` → fast-path do próprio chunk (3 ops) em ~85% das leituras.
 *   • Vértice de 16 B gravado com 4 stores u32 alinhados (sem branch, sem float).
 *   • Saída não-indexada (6 vértices/quad): zero index buffers para gerenciar.
 *   • Cabeçalho com AABB do chunk → frustum culling sem tocar nos voxels.
 *   • Dois segmentos (opaco / translúcido) → 2 draws por chunk, blend só no
 *     que precisa (água, vidro, gelo).
 */

import {
  VERTEX_BYTES, VERTS_PER_QUAD, MESH_HDR_BYTES,
  MH_OPAQUE_VERTS, MH_WATER_VERTS, MH_STATUS, MH_QUADS, MH_MIN_X, MH_MAX_X,
  FACE_PX, FACE_NX, FACE_PY, FACE_NY, FACE_PZ, FACE_NZ,
  BLK_AIR, BLK_GRASS, BLK_WATER, BLK_GLASS, BLK_ICE, BLK_LOG, BLK_LAMP,
  T_GRASS_TOP, T_GRASS_SIDE, T_DIRT, T_STONE, T_SAND, T_WATER, T_LOG_SIDE, T_LOG_TOP,
  T_LEAVES, T_SNOW, T_PLANK, T_BRICK, T_GLASS, T_COBBLE, T_GRAVEL, T_ICE, T_LAMP, T_BEDROCK,
} from "./constants";
import { getBlockFast, isOpaque, isTransparentStage, H_VOXELS_BASE } from "./world";

/** Camada do atlas para (bloco, face). Ordem espelhada em src/render/atlas.ts. */
@inline function tileOf(id: i32, face: i32): i32 {
  if (id == BLK_GRASS) return face == FACE_PY ? T_GRASS_TOP : (face == FACE_NY ? T_DIRT : T_GRASS_SIDE);
  if (id == BLK_LOG) return (face == FACE_PY || face == FACE_NY) ? T_LOG_TOP : T_LOG_SIDE;
  switch (id) {
    case 2: return T_DIRT;
    case 3: return T_STONE;
    case 4: return T_SAND;
    case 5: return T_WATER;
    case 7: return T_LEAVES;
    case 8: return T_SNOW;
    case 9: return T_PLANK;
    case 10: return T_BRICK;
    case 11: return T_GLASS;
    case 12: return T_COBBLE;
    case 13: return T_GRAVEL;
    case 14: return T_ICE;
    case 15: return T_LAMP;
    case 16: return T_BEDROCK;
    default: return T_STONE;
  }
}

/**
 * A face do bloco `self` é omitida quando o vizinho `other`:
 *   • é opaco (nunca se vê)                → omite
 *   • é o mesmo translúcido (água-água)    → omite (funde o volume)
 *   • é ar ou translúcido diferente        → desenha
 */
@inline function faceHidden(self: i32, other: i32): bool {
  if (isOpaque(other)) return true;
  if (other == BLK_AIR) return false;
  return other == self && isTransparentStage(self);
}

@inline function opaqueAt(hdr: i32, dp: usize, cx: i32, cy: i32, cz: i32, x: i32, y: i32, z: i32): i32 {
  return isOpaque(getBlockFast(hdr, dp, cx, cy, cz, x, y, z)) ? 1 : 0;
}

/** Vértice de 16 B: 4 words (posição u16, normal|AO|emissivo, uv|layer, luz). */
@inline function putVertex(p: usize, x: i32, y: i32, z: i32, n: i32, ao: i32, u: i32, v: i32, layer: i32, emissive: i32): void {
  store<u32>(p, u32(x | (y << 16)));
  store<u32>(p + 4, u32(z | (n << 16) | (ao << 19) | (emissive << 21)));
  store<u32>(p + 8, u32(u | (v << 8) | (layer << 16)));
  store<u32>(p + 12, u32(15 | (emissive << 8)));
}

/**
 * AO por canto (padrão 3-amostras): side1, side2 e canto, no plano do vizinho.
 * 0 = oclusão total, 3 = céu aberto. `su`/`sv` = lado do canto (-1/+1).
 */
@inline function cornerAO(
  hdr: i32, dp: usize, cx: i32, cy: i32, cz: i32,
  ox: i32, oy: i32, oz: i32, u: i32, v: i32, su: i32, sv: i32,
): i32 {
  const dux: i32 = u == 0 ? su : 0, duy: i32 = u == 1 ? su : 0, duz: i32 = u == 2 ? su : 0;
  const dvx: i32 = v == 0 ? sv : 0, dvy: i32 = v == 1 ? sv : 0, dvz: i32 = v == 2 ? sv : 0;
  const s1: i32 = opaqueAt(hdr, dp, cx, cy, cz, ox + dux, oy + duy, oz + duz);
  const s2: i32 = opaqueAt(hdr, dp, cx, cy, cz, ox + dvx, oy + dvy, oz + dvz);
  if (s1 != 0 && s2 != 0) return 0;
  const cc: i32 = opaqueAt(hdr, dp, cx, cy, cz, ox + dux + dvx, oy + duy + dvy, oz + duz + dvz);
  return 3 - (s1 + s2 + cc);
}

/**
 * Mesher. `scratchPtr` = 1 KB de rascunho na arena do worker (máscara 32×32).
 * @returns nº de vértices escritos (opacos + translúcidos).
 */
export function meshChunk(
  hdr: i32, slot: i32, cx: i32, cy: i32, cz: i32,
  outPtr: usize, cap: i32, scratchPtr: usize,
): i32 {
  const dp: usize = usize(load<i32>(hdr + H_VOXELS_BASE)) + usize(slot) * 32768;
  const wx0: i32 = cx << 5, wy0: i32 = cy << 5, wz0: i32 = cz << 5;

  // ── Early-out #1: chunk totalmente vazio (céu) ──────────────────────────
  // 2048 loads de 16 B (~0,02 ms) em vez de 400 mil leituras escalares.
  let anySolid: bool = false;
  for (let q: i32 = 0; q < 32768; q += 16) {
    if (v128.any_true(v128.load(dp + q))) { anySolid = true; break; }
  }
  if (!anySolid) {
    store<u32>(outPtr + MH_OPAQUE_VERTS, 0);
    store<u32>(outPtr + MH_WATER_VERTS, 0);
    store<u32>(outPtr + MH_STATUS, 0);
    store<u32>(outPtr + MH_QUADS, 0);
    for (let k: i32 = 0; k < 6; k++) store<f32>(outPtr + MH_MIN_X + k * 4, 0.0);
    for (let k2: i32 = 0; k2 < 6; k2++) store<f32>(outPtr + MH_MAX_X + k2 * 4, 0.0);
    return 0;
  }

  const mask: usize = scratchPtr;             // u8[1024]
  const opaqueStart: usize = outPtr + MESH_HDR_BYTES;
  const sectionCap: i32 = (cap - MESH_HDR_BYTES) / 2;
  const waterStart: usize = opaqueStart + sectionCap;

  let wOpaque: i32 = 0;   // bytes escritos
  let wWater: i32 = 0;
  let truncated: i32 = 0;
  let quads: i32 = 0;
  let minX: i32 = 0, minY: i32 = 0, minZ: i32 = 0, maxX: i32 = 0, maxY: i32 = 0, maxZ: i32 = 0;
  let hasAny: bool = false;

  for (let d: i32 = 0; d < 3; d++) {
    const u: i32 = (d + 1) % 3;
    const v: i32 = (d + 2) % 3;

    for (let dirIdx: i32 = 0; dirIdx < 2; dirIdx++) {
      const dir: i32 = dirIdx == 0 ? 1 : -1;
      const face: i32 = d == 0 ? (dir > 0 ? FACE_PX : FACE_NX)
                            : d == 1 ? (dir > 0 ? FACE_PY : FACE_NY)
                                     : (dir > 0 ? FACE_PZ : FACE_NZ);

      // s = plano na coordenada local do eixo d. 33 iterações (s=-1..31)
      // cobrem as duas bordas: faces do chunk nunca são esquecidas.
      for (let s: i32 = -1; s < 32; s++) {
        const ownerD: i32 = dir > 0 ? s : s + 1;   // voxel dono da face
        if (u32(ownerD) > 31) continue;            // face pertence ao vizinho
        const otherD: i32 = dir > 0 ? s + 1 : s;   // voxel do outro lado

        // ── Early-out #2: plano do dono inteiro é ar? ─────────────────────
        // Nos eixos Y e Z o plano contém 32 linhas contíguas de 32 B →
        // 64 loads SIMD descartam 1024 voxels de uma vez. (No eixo X as
        // linhas do plano têm passo 32/1024 em relação ao layout — o custo
        // do teste não compensa; segue pelo caminho escalar.)
        if (d != 0) {
          const planeOff: i32 = d == 1 ? (ownerD << 10) : (ownerD << 5);
          const rowStride: i32 = d == 1 ? 32 : 1024;
          let allAir: bool = true;
          for (let r: i32 = 0; r < 32; r++) {
            const rp: usize = dp + planeOff + r * rowStride;
            if (v128.any_true(v128.load(rp)) || v128.any_true(v128.load(rp + 16))) {
              allAir = false; break;
            }
          }
          if (allAir) continue;
        }

        // ── Máscara do plano (varredura u × v) ────────────────────────────
        let anyFace: bool = false;
        for (let j: i32 = 0; j < 32; j++) {
          for (let i: i32 = 0; i < 32; i++) {
            // coordenadas de mundo do voxel dono da face
            let sx: i32 = wx0, sy: i32 = wy0, sz: i32 = wz0;
            if (d == 0) sx += ownerD; else if (u == 0) sx += i; else sx += j;
            if (d == 1) sy += ownerD; else if (u == 1) sy += i; else sy += j;
            if (d == 2) sz += ownerD; else if (u == 2) sz += i; else sz += j;
            const self: i32 = getBlockFast(hdr, dp, cx, cy, cz, sx, sy, sz);
            if (self == BLK_AIR) { store<u8>(mask + (j * 32 + i), 0); continue; }
            let ox: i32 = wx0, oy: i32 = wy0, oz: i32 = wz0;
            if (d == 0) ox += otherD; else if (u == 0) ox += i; else ox += j;
            if (d == 1) oy += otherD; else if (u == 1) oy += i; else oy += j;
            if (d == 2) oz += otherD; else if (u == 2) oz += i; else oz += j;
            const other: i32 = getBlockFast(hdr, dp, cx, cy, cz, ox, oy, oz);
            if (faceHidden(self, other)) { store<u8>(mask + (j * 32 + i), 0); continue; }
            store<u8>(mask + (j * 32 + i), u8(self));  // máscara = id do bloco
            anyFace = true;
          }
        }
        if (!anyFace) continue;

        // ── Greedy merge de retângulos máximos ────────────────────────────
        for (let j: i32 = 0; j < 32; j++) {
          let i: i32 = 0;
          while (i < 32) {
            const m0: i32 = load<u8>(mask + (j * 32 + i));
            if (m0 == 0) { i++; continue; }
            let w: i32 = 1;                                  // largura (eixo u)
            while (i + w < 32 && load<u8>(mask + (j * 32 + i + w)) == m0) w++;
            let hh: i32 = 1;                                 // altura (eixo v)
            while (j + hh < 32) {
              let ok: bool = true;
              for (let k: i32 = 0; k < w; k++) {
                if (load<u8>(mask + ((j + hh) * 32 + i + k)) != m0) { ok = false; break; }
              }
              if (!ok) break;
              hh++;
            }
            for (let b: i32 = 0; b < hh; b++) {
              const row: usize = mask + ((j + b) * 32 + i);
              for (let a: i32 = 0; a < w; a++) store<u8>(row + a, 0);
            }

            // ── Geometria do quad (tudo escalar) ──────────────────────────
            const planeLocal: i32 = dir > 0 ? ownerD + 1 : ownerD;
            let qx: i32 = wx0, qy: i32 = wy0, qz: i32 = wz0;
            if (d == 0) qx += planeLocal; else if (u == 0) qx += i; else qx += j;
            if (d == 1) qy += planeLocal; else if (u == 1) qy += i; else qy += j;
            if (d == 2) qz += planeLocal; else if (u == 2) qz += i; else qz += j;

            // extensões do retângulo nos eixos u e v (em blocos de mundo)
            const euX: i32 = u == 0 ? w : 0, euY: i32 = u == 1 ? w : 0, euZ: i32 = u == 2 ? w : 0;
            const evX: i32 = v == 0 ? hh : 0, evY: i32 = v == 1 ? hh : 0, evZ: i32 = v == 2 ? hh : 0;

            // AO nos 4 cantos, amostrado no plano do vizinho
            const oX: i32 = (d == 0 ? otherD : (u == 0 ? i : j)) + wx0;
            const oY: i32 = (d == 1 ? otherD : (u == 1 ? i : j)) + wy0;
            const oZ: i32 = (d == 2 ? otherD : (u == 2 ? i : j)) + wz0;
            const a1: i32 = cornerAO(hdr, dp, cx, cy, cz, oX, oY, oZ, u, v, -1, -1);
            const a2: i32 = cornerAO(hdr, dp, cx, cy, cz, oX, oY, oZ, u, v, 1, -1);
            const a3: i32 = cornerAO(hdr, dp, cx, cy, cz, oX, oY, oZ, u, v, 1, 1);
            const a4: i32 = cornerAO(hdr, dp, cx, cy, cz, oX, oY, oZ, u, v, -1, 1);
            // Cantos canônicos: A=(0,0)→a1  B=(u,0)→a2  C=(u,v)→a3  D=(0,v)→a4
            // (o sentido da normal muda só a ORDEM de emissão, nunca o AO do canto)
            const aoA: i32 = a1, aoB: i32 = a2, aoC: i32 = a3, aoD: i32 = a4;
            const flip: bool = dir < 0;

            const layer: i32 = tileOf(m0, face);
            const emissive: i32 = m0 == BLK_LAMP ? 15 : 0;
            const isWater: bool = m0 == BLK_WATER || m0 == BLK_GLASS || m0 == BLK_ICE;

            let wr: i32 = isWater ? wWater : wOpaque;
            if (wr + VERTS_PER_QUAD * VERTEX_BYTES > sectionCap) {
              truncated = 1; i += w; continue;
            }

            // Cantos na ordem A(0,0) B(u,0) C(u,v) D(0,v) → cross(eu,ev)=+ed
            // normal +d: A,B,C,D | normal -d: A,D,C,B (garante CCW visto de fora)
            const bX: i32 = qx + euX, bY: i32 = qy + euY, bZ: i32 = qz + euZ;
            const cX: i32 = qx + euX + evX, cY: i32 = qy + euY + evY, cZ: i32 = qz + euZ + evZ;
            const dX: i32 = qx + evX, dY: i32 = qy + evY, dZ: i32 = qz + evZ;

            const dst: usize = (isWater ? waterStart : opaqueStart) + wr;
            let p: usize = dst;
            if (!flip) {
              putVertex(p, qx, qy, qz, face, aoA, 0, 0, layer, emissive); p += VERTEX_BYTES;
              putVertex(p, bX, bY, bZ, face, aoB, w, 0, layer, emissive); p += VERTEX_BYTES;
              putVertex(p, cX, cY, cZ, face, aoC, w, hh, layer, emissive); p += VERTEX_BYTES;
              putVertex(p, qx, qy, qz, face, aoA, 0, 0, layer, emissive); p += VERTEX_BYTES;
              putVertex(p, cX, cY, cZ, face, aoC, w, hh, layer, emissive); p += VERTEX_BYTES;
              putVertex(p, dX, dY, dZ, face, aoD, 0, hh, layer, emissive); p += VERTEX_BYTES;
            } else {
              putVertex(p, qx, qy, qz, face, aoA, 0, 0, layer, emissive); p += VERTEX_BYTES;
              putVertex(p, dX, dY, dZ, face, aoD, 0, hh, layer, emissive); p += VERTEX_BYTES;
              putVertex(p, cX, cY, cZ, face, aoC, w, hh, layer, emissive); p += VERTEX_BYTES;
              putVertex(p, qx, qy, qz, face, aoA, 0, 0, layer, emissive); p += VERTEX_BYTES;
              putVertex(p, cX, cY, cZ, face, aoC, w, hh, layer, emissive); p += VERTEX_BYTES;
              putVertex(p, bX, bY, bZ, face, aoB, w, 0, layer, emissive); p += VERTEX_BYTES;
            }
            wr += VERTS_PER_QUAD * VERTEX_BYTES;
            if (isWater) wWater = wr; else wOpaque = wr;
            quads++;

            // AABB do chunk (frustum culling no lado do render)
            if (!hasAny) {
              minX = qx; minY = qy; minZ = qz; maxX = cX; maxY = cY; maxZ = cZ; hasAny = true;
            } else {
              if (qx < minX) minX = qx; if (qy < minY) minY = qy; if (qz < minZ) minZ = qz;
              if (cX > maxX) maxX = cX; if (cY > maxY) maxY = cY; if (cZ > maxZ) maxZ = cZ;
            }
            i += w;
          }
        }
      }
    }
  }

  store<u32>(outPtr + MH_OPAQUE_VERTS, u32(wOpaque / VERTEX_BYTES));
  store<u32>(outPtr + MH_WATER_VERTS, u32(wWater / VERTEX_BYTES));
  store<u32>(outPtr + MH_STATUS, u32(truncated));
  store<u32>(outPtr + MH_QUADS, u32(quads));
  store<u32>(outPtr + 40, u32(slot));               // slot do chunk (p/ a main thread)
  store<f32>(outPtr + MH_MIN_X, f32(minX));
  store<f32>(outPtr + MH_MIN_X + 4, f32(minY));
  store<f32>(outPtr + MH_MIN_X + 8, f32(minZ));
  store<f32>(outPtr + MH_MAX_X, f32(maxX));
  store<f32>(outPtr + MH_MAX_X + 4, f32(maxY));
  store<f32>(outPtr + MH_MAX_X + 8, f32(maxZ));
  return (wOpaque + wWater) / VERTEX_BYTES;
}
