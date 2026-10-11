/**
 * ============================================================================
 *  ENTRADA — teclado, mouse (pointer lock) e toque
 * ============================================================================
 *  Coleta eventos e entrega um "snapshot" por frame (sem alocação): a engine
 *  consulta `isDown(code)` e lê `mouseDX/mouseDY` acumulados.
 *  Pointer lock é pedido no clique e re-solicitado se o navegador sair dele.
 */

import type { PlayerInput } from './player';

export type KeyListener = (code: string, down: boolean, repeat: boolean) => void;

export class Input {
  private down = new Set<string>();
  private listeners: KeyListener[] = [];
  mouseDX = 0;
  mouseDY = 0;
  wheel = 0;
  mouse0 = false;   // botão esquerdo (quebrar)
  mouse2 = false;   // botão direito (colocar)
  mouse0Pressed = false;
  mouse2Pressed = false;
  locked = false;
  touchLook = { active: false, id: -1, x: 0, y: 0 };
  touchMove = { active: false, id: -1, ox: 0, oy: 0, dx: 0, dy: 0 };
  sensitivity = 1;

  constructor(private canvas: HTMLCanvasElement) {
    addEventListener('keydown', (e: KeyboardEvent) => {
      if (e.repeat) { this.emit(e.code, true, true); return; }
      this.down.add(e.code);
      this.emit(e.code, true, false);
      if (['Space', 'Tab', 'F3', 'F5'].includes(e.code)) e.preventDefault();
    });
    addEventListener('keyup', (e: KeyboardEvent) => { this.down.delete(e.code); this.emit(e.code, false, false); });
    addEventListener('blur', () => this.down.clear());

    canvas.addEventListener('mousedown', (e: MouseEvent) => {
      if (!this.locked) { this.requestLock(); return; }
      if (e.button === 0) { this.mouse0 = true; this.mouse0Pressed = true; }
      if (e.button === 2) { this.mouse2 = true; this.mouse2Pressed = true; }
    });
    addEventListener('mouseup', (e: MouseEvent) => {
      if (e.button === 0) this.mouse0 = false;
      if (e.button === 2) this.mouse2 = false;
    });
    canvas.addEventListener('contextmenu', (e: MouseEvent) => e.preventDefault());
    addEventListener('mousemove', (e: MouseEvent) => {
      if (!this.locked) return;
      this.mouseDX += e.movementX * this.sensitivity;
      this.mouseDY += e.movementY * this.sensitivity;
    });
    document.addEventListener('pointerlockchange', () => { this.locked = document.pointerLockElement === canvas; });
    addEventListener('wheel', (e: WheelEvent) => { this.wheel += Math.sign(e.deltaY); }, { passive: true });

    // ── Toque: metade esquerda = analógico de movimento, direita = olhar ──
    canvas.addEventListener('touchstart', (e) => {
      for (const t of Array.from(e.changedTouches)) {
        if (t.clientX < innerWidth / 2 && !this.touchMove.active) {
          this.touchMove = { active: true, id: t.identifier, ox: t.clientX, oy: t.clientY, dx: 0, dy: 0 };
        } else if (!this.touchLook.active) {
          this.touchLook = { active: true, id: t.identifier, x: t.clientX, y: t.clientY };
        }
      }
      e.preventDefault();
    }, { passive: false });
    canvas.addEventListener('touchmove', (e) => {
      for (const t of Array.from(e.changedTouches)) {
        if (t.identifier === this.touchMove.id) {
          this.touchMove.dx = Math.max(-60, Math.min(60, t.clientX - this.touchMove.ox)) / 60;
          this.touchMove.dy = Math.max(-60, Math.min(60, t.clientY - this.touchMove.oy)) / 60;
        } else if (t.identifier === this.touchLook.id) {
          this.mouseDX += (t.clientX - this.touchLook.x) * 1.4;
          this.mouseDY += (t.clientY - this.touchLook.y) * 1.4;
          this.touchLook.x = t.clientX; this.touchLook.y = t.clientY;
        }
      }
      e.preventDefault();
    }, { passive: false });
    const endTouch = (e: TouchEvent) => {
      for (const t of Array.from(e.changedTouches)) {
        if (t.identifier === this.touchLook.id) this.touchLook = { active: false, id: -1, x: 0, y: 0 };
        if (t.identifier === this.touchMove.id) this.touchMove = { active: false, id: -1, ox: 0, oy: 0, dx: 0, dy: 0 };
      }
    };
    canvas.addEventListener('touchend', endTouch);
    canvas.addEventListener('touchcancel', endTouch);
  }

  requestLock(): void {
    const promise = this.canvas.requestPointerLock({ unadjustedMovement: true } as PointerLockOptions) as unknown as Promise<void> | undefined;
    if (promise && typeof promise.catch === 'function') promise.catch(() => this.canvas.requestPointerLock());
  }

  onKey(fn: KeyListener): void { this.listeners.push(fn); }
  private emit(code: string, down: boolean, repeat: boolean): void {
    for (const l of this.listeners) l(code, down, repeat);
  }

  isDown(code: string): boolean { return this.down.has(code); }

  anyDown(codes: string[]): boolean {
    for (const c of codes) if (this.down.has(c)) return true;
    return false;
  }

  /** Snapshot para o jogador (mesmo objeto reutilizado — zero alocação). */
  snapshot(out: PlayerInput): PlayerInput {
    const t = this.touchMove;
    out.forward = (this.isDown('KeyW') || this.isDown('ArrowUp') ? 1 : 0) + Math.max(0, -t.dy);
    out.back = (this.isDown('KeyS') || this.isDown('ArrowDown') ? 1 : 0) + Math.max(0, t.dy);
    out.left = (this.isDown('KeyA') || this.isDown('ArrowLeft') ? 1 : 0) + Math.max(0, -t.dx);
    out.right = (this.isDown('KeyD') || this.isDown('ArrowRight') ? 1 : 0) + Math.max(0, t.dx);
    out.jump = this.isDown('Space');
    out.run = this.anyDown(['ShiftLeft', 'ShiftRight']);
    out.crouch = this.anyDown(['ControlLeft', 'KeyC']);
    out.fly = this.isDown('KeyV');
    out.mouseDX = this.mouseDX;
    out.mouseDY = this.mouseDY;
    this.mouseDX = 0; this.mouseDY = 0;
    return out;
  }

  endFrame(): void {
    this.mouse0Pressed = false;
    this.mouse2Pressed = false;
    this.wheel = 0;
  }
}
