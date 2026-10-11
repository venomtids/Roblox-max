/**
 * ============================================================================
 *  JOGADOR — controlador FPS/TPS com física delegada ao WASM
 * ============================================================================
 *  A colisão NÃO é feita aqui: chamamos `core.sweepBox` (WASM, bissecção por
 *  eixo) uma vez por passo. Este arquivo cuida apenas de:
 *   • entrada (teclado/mouse/toque) com buffer de eventos;
 *   • aceleração, atrito, pulo, coyote time, corrida, agachar e voo;
 *   • natação e empuxo (a água é detectada pelo WASM e devolvida em flag);
 *   • câmera (yaw/pitch) com suavização e head-bob discreto.
 *
 *  Custo por passo: ~1 chamada WASM (~5 µs) + aritmética trivial.
 */

import type { Core } from '../core/wasm';
import { SharedWorld, SEA_LEVEL } from '../core/memory';
import { clamp } from '../core/math';

export const PLAYER_HALF = 0.3;      // meio-largura (0.6 de largura)
export const PLAYER_HEIGHT = 1.8;    // altura total (câmera em ~1.62)
const EYE = 1.62;

export interface PlayerInput {
  forward: number; back: number; left: number; right: number;
  jump: boolean; run: boolean; crouch: boolean; fly: boolean;
  mouseDX: number; mouseDY: number;
}

export class Player {
  pos = new Float32Array([0, 0, 0]);
  vel = new Float32Array([0, 0, 0]);
  yaw = 0; pitch = 0;
  onGround = false;
  inWater = false;
  flying = false;
  crouching = false;
  spawnPoint = new Float32Array([0, 0, 0]);
  private coyote = 0;
  private jumpBuffer = 0;
  private bob = 0;
  private scratch: number;
  private stepDt = 1 / 60;
  /** View reutilizada sobre a memória compartilhada — nada é alocado por passo. */
  private outF32: Float32Array;

  constructor(private world: SharedWorld, private core: Core, scratchPtr: number) {
    this.scratch = scratchPtr;
    this.outF32 = new Float32Array(world.sab, scratchPtr, 4);
  }

  /** Posiciona o jogador na superfície livre mais próxima (spawn seguro). */
  spawn(x: number, z: number): void {
    const y = this.core.surfaceY(this.world.headerPtr, x, z);
    let sy = y;
    for (let dy = 0; dy < 24; dy++) {
      if (this.core.isSpawnClear(this.world.headerPtr, x, y + dy, z)) { sy = y + dy; break; }
    }
    this.pos[0] = x + 0.5; this.pos[1] = sy + 0.1; this.pos[2] = z + 0.5;
    this.spawnPoint.set(this.pos);
    this.vel.fill(0);
  }

  respawn(): void {
    this.pos.set(this.spawnPoint);
    this.vel.fill(0);
  }

  /** Um passo de simulação (chamado a 60 Hz fixos pela engine). */
  step(input: PlayerInput, dt: number): void {
    this.stepDt = dt;
    // ── Orientação ───────────────────────────────────────────────────────
    const sens = 0.0022;
    this.yaw -= input.mouseDX * sens;
    this.pitch = clamp(this.pitch - input.mouseDY * sens, -1.5533, 1.5533);

    // ── Direções no plano (yaw) ──────────────────────────────────────────
    const sy = Math.sin(this.yaw), cy = Math.cos(this.yaw);
    const fwd = (input.forward - input.back);
    const strafe = (input.right - input.left);
    let wishX = (-sy * fwd) + (cy * strafe);
    let wishZ = (-cy * fwd) + (-sy * strafe);
    const wl = Math.hypot(wishX, wishZ);
    if (wl > 1e-4) { wishX /= wl; wishZ /= wl; }

    this.crouching = input.crouch && !input.fly;
    const speed = (this.crouching ? 2.4 : input.run ? 8.4 : 5.2) * (this.inWater ? 0.6 : 1);

    // ── Voo criativo (duplo toque em V liga/desliga) ─────────────────────
    if (input.fly) {
      this.flying = true;
    } else if (this.flying && this.onGround) {
      this.flying = false;
    }

    if (this.flying) {
      const acc = 42 * dt;
      this.vel[0] += wishX * acc; this.vel[2] += wishZ * acc;
      this.vel[1] += ((input.jump ? 1 : 0) - (input.crouch ? 1 : 0)) * acc;
      const damp = Math.pow(0.0008, dt);
      this.vel[0] *= damp; this.vel[1] *= damp; this.vel[2] *= damp;
      const sp = Math.hypot(this.vel[0], this.vel[1], this.vel[2]);
      const maxSp = 14;
      if (sp > maxSp) { const k = maxSp / sp; this.vel[0] *= k; this.vel[1] *= k; this.vel[2] *= k; }
    } else {
      // ── Aceleração no solo/ar (modelo "quake-like": resposta imediata) ──
      const accel = this.onGround ? 14.0 : (this.inWater ? 8.0 : 5.0);
      const targetX = wishX * speed, targetZ = wishZ * speed;
      const k = Math.min(1, accel * dt);
      this.vel[0] += (targetX - this.vel[0]) * k;
      this.vel[2] += (targetZ - this.vel[2]) * k;
      if (wl < 1e-4 && this.onGround) {
        const fric = Math.pow(0.0001, dt);
        this.vel[0] *= fric; this.vel[2] *= fric;
      }

      // ── Pulo com coyote time + input buffer (game feel) ────────────────
      this.jumpBuffer = input.jump ? 0.14 : Math.max(0, this.jumpBuffer - dt);
      this.coyote = this.onGround ? 0.12 : Math.max(0, this.coyote - dt);
      if (this.jumpBuffer > 0 && (this.coyote > 0 || this.inWater)) {
        this.vel[1] = this.inWater ? 5.6 : 9.4;
        this.coyote = 0; this.jumpBuffer = 0;
        this.onGround = false;
      }
      if (!input.jump && this.vel[1] > 0) this.vel[1] -= 26 * dt;   // pulo variável
    }

    // ── Colisão + integração: UMA chamada ao WASM resolve tudo ───────────
    const hx = PLAYER_HALF, hz = PLAYER_HALF;
    const hyTotal = PLAYER_HEIGHT / 2;
    const centerY = this.pos[1] + hyTotal;
    const flags = this.core.sweepBox(
      this.world.headerPtr,
      this.pos[0], centerY, this.pos[2],
      this.vel[0] * dt, this.vel[1] * dt, this.vel[2] * dt,
      hx, hyTotal, hz, this.scratch,
    );
    const f32 = this.outF32;
    this.pos[0] = f32[0];
    this.pos[1] = f32[1] - hyTotal;    // volta a ser "pés" (convenção do player)
    this.pos[2] = f32[2];

    this.onGround = (flags & 1) !== 0;             // COL_GROUND
    this.inWater = (flags & 2) !== 0;              // COL_WATER
    if (this.onGround) { if (this.vel[1] < 0) this.vel[1] = 0; }
    if (flags & 4) this.vel[0] = 0;                // COL_X
    if (flags & 16) this.vel[2] = 0;               // COL_Z
    if ((flags & 32) && this.vel[1] > 0) this.vel[1] = 0;  // COL_CEIL

    // ── Head-bob discreto (sensação de passos) ───────────────────────────
    const hsp = Math.hypot(this.vel[0], this.vel[2]);
    if (this.onGround && hsp > 0.6) this.bob += dt * hsp * 1.35;
    else this.bob += dt * 0.6;

    // ── Rede de segurança: se cair fora do mundo, volta ao spawn ─────────
    if (this.pos[1] < -8) this.respawn();
  }

  /** Posição da câmera (olhos), com altura de agachar e head-bob. */
  eyePosition(out: Float32Array): void {
    const crouchDrop = this.crouching ? 0.45 : 0;
    const bobY = this.onGround ? Math.sin(this.bob * 2) * 0.035 * (Math.hypot(this.vel[0], this.vel[2]) > 0.6 ? 1 : 0) : 0;
    const bobX = this.onGround ? Math.cos(this.bob) * 0.02 : 0;
    const sy = Math.sin(this.yaw), cy = Math.cos(this.yaw);
    out[0] = this.pos[0] + cy * bobX;
    out[1] = this.pos[1] + EYE - crouchDrop + bobY;
    out[2] = this.pos[2] - sy * bobX;
  }

  /** Direção de visão (unitária) — usada por raycast e projéteis. */
  lookDir(out: Float32Array): void {
    const cp = Math.cos(this.pitch), sp = Math.sin(this.pitch);
    const sy = Math.sin(this.yaw), cy = Math.cos(this.yaw);
    out[0] = -sy * cp; out[1] = sp; out[2] = -cy * cp;
  }

  get underWater(): boolean { return this.inWater && this.pos[1] + EYE < SEA_LEVEL + 0.2; }
  get speed(): number { return Math.hypot(this.vel[0], this.vel[1], this.vel[2]); }
  get dt(): number { return this.stepDt; }
}
