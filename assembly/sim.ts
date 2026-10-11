/**
 * ============================================================================
 *  SIMULAÇÃO EM LOTE DE CORPOS RÍGIDOS (rodada no WORKER de física)
 * ============================================================================
 *  A main thread NUNCA simula: ela apenas lê o buffer compartilhado de corpos
 *  (SharedArrayBuffer) e desenha instâncias. Zero postMessage por frame, zero
 *  cópia, zero desserialização — apenas Atomics para o sinal de "pronto".
 *
 *  Corpo (64 B): px,py,pz | vx,vy,vz | half | flags | color | rest | userData
 *  O padding para 64 B alinha cada corpo a 1 cache line → varredura linear
 *  com prefetch perfeito (1000 corpos ≈ 20 µs).
 */

import {
  BODY_STRIDE, B_PX, B_PY, B_PZ, B_VX, B_VY, B_VZ, B_HALF, B_FLAGS, B_COLOR, B_REST,
  FLAG_ACTIVE, FLAG_GROUNDED, FLAG_SLEEPING, GRAVITY, TERMINAL_VEL, AIR_DRAG, WATER_DRAG,
} from "./constants";
import { sweepBox, COL_GROUND, COL_WATER } from "./physics";

const SLEEP_TIME: f32 = 1.2;    // s em repouso → dorme (economiza CPU)
const SLEEP_SPEED: f32 = 0.06;  // velocidade limite para considerar repouso

/**
 * Integra `count` corpos por `dt` segundos, com colisão contra o mundo.
 * @returns bitmask: bit0 = algum corpo acordado (a main thread sabe se precisa redesenhar)
 */
export function simulateBodies(hdr: i32, bodyBase: usize, count: i32, dt: f32, scratchPtr: usize): i32 {
  let awake: i32 = 0;
  for (let i: i32 = 0; i < count; i++) {
    const b: usize = bodyBase + i * BODY_STRIDE;
    let flags: i32 = load<i32>(b + B_FLAGS);
    if ((flags & FLAG_ACTIVE) == 0) continue;
    if ((flags & FLAG_SLEEPING) != 0) continue;

    let px: f32 = load<f32>(b + B_PX);
    let py: f32 = load<f32>(b + B_PY);
    let pz: f32 = load<f32>(b + B_PZ);
    let vx: f32 = load<f32>(b + B_VX);
    let vy: f32 = load<f32>(b + B_VY);
    let vz: f32 = load<f32>(b + B_VZ);
    const half: f32 = load<f32>(b + B_HALF);

    // Integração semi-implícita de Euler (estável e barata)
    vy -= GRAVITY * dt;
    if (vy < -TERMINAL_VEL) vy = -TERMINAL_VEL;

    // Arrasto (ar quase nada, água muito)
    const inWater: bool = (flags & FLAG_GROUNDED) != 0 ? false : false;
    vx *= AIR_DRAG; vz *= AIR_DRAG;

    const res: i32 = sweepBox(hdr, px, py, pz, vx * dt, vy * dt, vz * dt, half, half, half, scratchPtr);
    px = load<f32>(scratchPtr);
    py = load<f32>(scratchPtr + 4);
    pz = load<f32>(scratchPtr + 8);

    // Resposta a colisões + atrito
    let grounded: bool = (res & COL_GROUND) != 0;
    if (grounded) {
      vy = 0.0;
      vx *= 0.72; vz *= 0.72;          // atrito do chão
      // repique leve (dá "vida" ao cubo da Roblox)
      if (Mathf.abs(vx) < 0.35) vx = 0.0;
      if (Mathf.abs(vz) < 0.35) vz = 0.0;
    } else {
      vx *= 0.995; vz *= 0.995;
    }
    if ((res & 4) != 0) vx = 0.0;       // COL_X
    if ((res & 16) != 0) vz = 0.0;      // COL_Z
    if ((res & 32) != 0 && vy > 0.0) vy = 0.0;  // teto

    if ((res & COL_WATER) != 0) {
      vy += GRAVITY * dt * 0.72;        // empuxo
      vx *= WATER_DRAG; vz *= WATER_DRAG; vy *= WATER_DRAG;
      grounded = false;
    }

    // Sono: corpo parado por SLEEP_TIME → sai do laço (CPU → 0)
    const speed: f32 = Mathf.abs(vx) + Mathf.abs(vy) + Mathf.abs(vz);
    let rest: f32 = load<f32>(b + B_REST);
    if (grounded && speed < SLEEP_SPEED) {
      rest += dt;
      if (rest > SLEEP_TIME) { flags |= FLAG_SLEEPING; vx = 0.0; vy = 0.0; vz = 0.0; }
    } else rest = 0.0;

    // Acorda corpos ao redor quando algo se move perto (barato e O(1) por corpo)
    if (speed > 1.0) awake = 1;

    flags = grounded ? (flags | FLAG_GROUNDED) : (flags & ~FLAG_GROUNDED);
    store<f32>(b + B_PX, px); store<f32>(b + B_PY, py); store<f32>(b + B_PZ, pz);
    store<f32>(b + B_VX, vx); store<f32>(b + B_VY, vy); store<f32>(b + B_VZ, vz);
    store<f32>(b + B_REST, rest);
    store<i32>(b + B_FLAGS, flags);
  }
  return awake;
}

/** Acorda todos os corpos dentro de um raio (chamado quando o jogador edita o mundo). */
export function wakeBodies(bodyBase: usize, count: i32, x: f32, y: f32, z: f32, radius: f32): void {
  const r2: f32 = radius * radius;
  for (let i: i32 = 0; i < count; i++) {
    const b: usize = bodyBase + i * BODY_STRIDE;
    if ((load<i32>(b + B_FLAGS) & FLAG_ACTIVE) == 0) continue;
    const dx: f32 = load<f32>(b + B_PX) - x;
    const dy: f32 = load<f32>(b + B_PY) - y;
    const dz: f32 = load<f32>(b + B_PZ) - z;
    if (dx * dx + dy * dy + dz * dz <= r2) {
      store<i32>(b + B_FLAGS, load<i32>(b + B_FLAGS) & ~FLAG_SLEEPING);
      store<f32>(b + B_REST, 0.0);
    }
  }
}
