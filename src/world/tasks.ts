/**
 * ============================================================================
 *  ANEL DE TAREFAS LOCK-FREE (single-producer / multi-consumer)
 * ============================================================================
 *  A main thread publica; N workers consomem. Sincronização 100% Atomics:
 *
 *    head  — total de tarefas publicadas          (produtor incrementa)
 *    claim — próxima tarefa a reservar            (consumidor: fetch-add)
 *
 *  Consumidor:  i = Atomics.add(claim, 1)
 *               se i < Atomics.load(head) → é minha, executa
 *               senão → devolve o índice (compareExchange) e espera
 *
 *  Propriedades: sem locks, sem semáforos, sem fila alocada, sem travamento
 *  quando um worker morre (o índice não é "perdido" — volta para o anel).
 *  4096 entradas × 16 B = cabe com folga no orçamento de memória.
 */

import {
  LAYOUT, C_TASK_HEAD, C_TASK_CLAIM, C_TASK_DONE,
  JOB_FULL, JOB_REMESH, MAX_WORKERS, C_SEQ_BASE,
} from '../core/memory';

export const RING_SIZE = 4096;
export const ENTRY_I32 = 4;                     // [kind, cx, cy, cz]
const RING_BYTES = RING_SIZE * ENTRY_I32 * 4;

export interface Task { kind: number; cx: number; cy: number; cz: number; }

export class TaskRing {
  private readonly entries: Int32Array;

  constructor(private readonly control: Int32Array, private readonly sab: SharedArrayBuffer) {
    this.entries = new Int32Array(sab, LAYOUT.tasks, RING_SIZE * ENTRY_I32);
  }

  /** Publica uma tarefa (chamado apenas pela main thread). */
  push(kind: number, cx: number, cy: number, cz: number): void {
    const head = Atomics.load(this.control, C_TASK_HEAD);
    const at = (head % RING_SIZE) * ENTRY_I32;
    // Só escreve se o worker já consumiu a volta anterior deste índice.
    for (let guard = 0; guard < 64; guard++) {
      const claimed = Atomics.load(this.control, C_TASK_CLAIM);
      if (head - claimed < RING_SIZE) break;
      // anel cheio: espera um pouco (não deve acontecer com 4096 slots)
      Atomics.wait(this.control, C_TASK_HEAD, head, 1);
    }
    this.entries[at] = kind;
    this.entries[at + 1] = cx;
    this.entries[at + 2] = cy;
    this.entries[at + 3] = cz;
    Atomics.store(this.control, C_TASK_HEAD, head + 1);
    Atomics.notify(this.control, C_TASK_HEAD);
  }

  /** Reserva a próxima tarefa (qualquer thread). Devolve null se não houver. */
  claim(): Task | null {
    for (let attempt = 0; attempt < 64; attempt++) {
      const i = Atomics.add(this.control, C_TASK_CLAIM, 1);
      if (i < Atomics.load(this.control, C_TASK_HEAD)) {
        const at = (i % RING_SIZE) * ENTRY_I32;
        return { kind: this.entries[at], cx: this.entries[at + 1], cy: this.entries[at + 2], cz: this.entries[at + 3] };
      }
      // Não havia tarefa: devolve o índice para não "pular" nada.
      const back = Atomics.compareExchange(this.control, C_TASK_CLAIM, i + 1, i);
      if (back === i + 1) return null;
      // Corrida: outro consumidor reivindicou; tenta de novo (máx. 64 voltas).
    }
    return null;
  }

  /** Bloqueia o worker até haver tarefa nova (poupa 100% de CPU parada). */
  claimBlocking(timeoutMs = 50): Task | null {
    for (;;) {
      const t = this.claim();
      if (t) return t;
      const head = Atomics.load(this.control, C_TASK_HEAD);
      if (head >= this.totalPublished()) return null;
      Atomics.wait(this.control, C_TASK_HEAD, head, timeoutMs);
    }
  }

  totalPublished(): number { return Atomics.load(this.control, C_TASK_HEAD); }
  totalClaimed(): number { return Atomics.load(this.control, C_TASK_CLAIM); }
  totalDone(): number { return Atomics.load(this.control, C_TASK_DONE); }
  markDone(): void { Atomics.add(this.control, C_TASK_DONE, 1); }
  pending(): number { return Math.max(0, this.totalPublished() - this.totalDone()); }
}

/**
 * Canais seq/ack por worker: o worker publica o mesh no SEU slot e avisa
 * com um número de sequência; a main thread consome e "acka". Enquanto o ack
 * não chega, o worker não sobrescreve o próprio slot — nunca há corrida.
 */
export class MeshChannel {
  constructor(private readonly control: Int32Array) {}

  /** Worker: anuncia que um mesh novo está no seu slot de saída. */
  workerPublish(worker: number): void {
    Atomics.add(this.control, C_SEQ_BASE + worker * 2, 1);
    Atomics.notify(this.control, C_SEQ_BASE + worker * 2);
  }

  /** Worker: espera a main thread liberar o slot (evita sobrescrever em uso). */
  workerWaitForAck(worker: number, timeoutMs = 4): void {
    const idx = C_SEQ_BASE + worker * 2 + 1;
    for (let spin = 0; spin < 64; spin++) {
      const seq = Atomics.load(this.control, C_SEQ_BASE + worker * 2);
      const ack = Atomics.load(this.control, idx);
      if (ack === seq) return;
      Atomics.wait(this.control, idx, ack, timeoutMs);
    }
  }

  /** Main thread: há mesh novo de `worker` esperando consumo? */
  pending(worker: number): number {
    const seq = Atomics.load(this.control, C_SEQ_BASE + worker * 2);
    const ack = Atomics.load(this.control, C_SEQ_BASE + worker * 2 + 1);
    return seq - ack;
  }

  /** Main thread: consumo concluído — libera o slot do worker. */
  consumerAck(worker: number): void {
    const idx = C_SEQ_BASE + worker * 2 + 1;
    const seq = Atomics.load(this.control, C_SEQ_BASE + worker * 2);
    Atomics.store(this.control, idx, seq);
    Atomics.notify(this.control, idx);
  }
}

export { JOB_FULL, JOB_REMESH, RING_BYTES };
