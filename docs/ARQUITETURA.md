# Arquitetura profunda — ROBLOX-MAX

Documento para quem vai **mexer** no engine. Explica cada decisão que custa performance se tomada errado.

---

## 1. O princípio que governa tudo

> **A main thread não trabalha: ela decide.**

Toda tarefa pesada (gerar terreno, gerar malha, simular corpos) acontece em outro lugar — workers ou GPU.
A main thread, por frame, faz apenas:

1. Lê input (~µs);
2. Chama **uma** função WASM de física do jogador (~5 µs);
3. Sinaliza os ticks de física dos corpos (1 `Atomics.add`, ~50 ns);
4. Drena meshes prontos (~1 µs por chunk, sem cópia);
5. Culling + emissão de draw calls (~50–200 µs);
6. Atualiza o HUD (~10 `textContent`, a 5 Hz).

Orçamento real observado: **0,6–1,5 ms de CPU por frame**, o que deixa 15+ ms de folga para 60 FPS estáveis.

---

## 2. Memória compartilhada: o "barramento" do engine

Um único `WebAssembly.Memory({ shared: true })` de **32 MB** é a RAM do jogo. Ele é:

- **importado** pelo núcleo WASM (`--importMemory --sharedMemory`), e
- **clonado via `postMessage`** para os workers (structured clone de `WebAssembly.Memory`).

Resultado: todas as threads leem/escrevem os **mesmos bytes**, sem cópia, sem serialização, sem `postMessage` de geometria.

```
0x001000  HEADER      i32[8]    voxelsBase, tableBase, sizeX/Y/Z, sea, seed, ver
0x002000  TABLE       i32[640]  slot do chunk (índice linear) ou −1 (não pronto)
0x003000  CHUNKSTATE  i32[640]  0=livre 1=na fila 2=pronto 3=sujo
0x004000  CONTROL     i32[256]  head/claim/done da fila, seq/ack de mesh, edit epoch,
                                pedido de tick de física, dt (f32 no slot 10)
0x008000  TASKS       4096 × 16 B  anel lock-free: [kind, cx, cy, cz]
0x020000  BODIES      4096 × 64 B  corpos rígidos (1 corpo = 1 cache line)
0x060000  ARENAS      8 × 64 KB    rascunho por worker (máscara do mesher, saída de física)
0x100000  MESHOUT     8 × 640 KB   saída do mesher (64 B de cabeçalho + 2 × 256 KB)
0x800000  VOXELS      640 × 32 KB  = 20 MB de blocos (1 byte por voxel)
```

Índice de voxel dentro do chunk: `idx = x | (z << 5) | (y << 10)` — **x é o eixo rápido**.
Essa escolha é deliberada: o mesher varre slices contíguos de 32 bytes, o que permite descartar
16 voxels por instrução SIMD (`v128.any_true`).

---

## 3. Modelo de concorrência

### 3.1 Anel de tarefas lock-free (`src/world/tasks.ts`)

Produtor único (main thread), N consumidores (workers):

```
push:   escreve em entries[head % N];  Atomics.store(head, head+1); Atomics.notify
claim:  i = Atomics.add(claim, 1)
        se i < Atomics.load(head) → é minha
        senão → Atomics.compareExchange(claim, i+1, i)  // devolve o índice
                Atomics.wait(head, headAtual, timeout)  // dorme de verdade
```

Propriedades: sem mutex, sem semáforo, sem `postMessage`; a tarefa nunca é perdida nem duplicada;
um worker morto não trava a fila (o índice volta ao anel).

### 3.2 Canal seq/ack de mesh

O worker escreve o mesh **no seu próprio slot de saída** e publica um número de sequência:
`Atomics.add(CONTROL[16 + i*2], 1)`. A main thread, ao subir o mesh para a VRAM, faz o *ack*
(`Atomics.store(...+1, seq)`). Enquanto o ack não chega, o worker **não sobrescreve** o slot.
Isso elimina a corrida entre "upload em andamento" e "próximo mesh".

### 3.3 Física assíncrona com backpressure

A main thread pede até 3 passos à frente e segue renderizando com as posições do **último passo
concluído**. Se o worker de física atrasar, a cena repete o último estado em vez de travar o
frame — o padrão que engines comerciais usam para física fora da main thread.

---

## 4. Núcleo WebAssembly (`assembly/`)

Compilado com:

```
asc assembly/index.ts --optimize --shrinkLevel 1 --noAssert
    --enable simd --enable bulk-memory --enable threads
    --importMemory --sharedMemory --maximumMemory 512
    --runtime stub --exportRuntime
```

- `--runtime stub` → **sem GC, sem alocador**. Todo vetor é um ponteiro de arena calculado pela main thread.
- `--enable simd` → kernels de 16 bytes por instrução.
- Nenhuma variável de módulo mutável: as instâncias são *stateless* e thread-safe por construção
  (o contexto entra por parâmetro: `hdr`, `outPtr`, `scratchPtr`).
- 11 KB de binário: o download é irrelevante e compila em paralelo (`instantiateStreaming`).

### 4.1 Greedy mesher (`assembly/mesh.ts`)

Algoritmo por eixo/sentido: compara dois planos de 32×32 voxels, monta máscara `u8[1024]`,
funde retângulos máximos. Otimizações que importam:

| Técnica | Ganho |
|---|---|
| `getBlockFast` (fast-path do próprio chunk, 3 ops) | ~85% das leituras sem consultar a tabela |
| Early-out SIMD de chunk vazio (`v128.any_true` em 2048 posições) | chunks de céu: 0,02 ms em vez de 1 ms |
| Early-out SIMD de plano vazio (eixos Y/Z: 64 loads de 16 B) | elimina ~60% das varreduras de borda |
| Zero arrays/objetos (só escalares empilhados) | nenhuma chamada a `__new` no loop mais quente |
| Vértice de 16 B em 4 stores u32 alinhados | sem branch, sem float, sem conversão |
| Saída não-indexada (6 vértices/quad) | zero index buffers para gerenciar |

**Resultado medido: 649k vértices / 108k quads** para um mundo de 384×128×384 — contra
~1,4 M vértices do método ingênuo (1 quad por face visível).

### 4.2 Física (`assembly/physics.ts`)

- **`sweepBox`**: move por eixo com **bissecção de 12 iterações** em vez de "snap no voxel".
  Por que: um corpo a 60 blocos/s percorre 1 bloco por frame; um snap simples encontra o voxel
  errado e empurra o corpo para dentro da parede (o bug clássico de colisão em voxel).
  A bissecção acha a maior posição livre com precisão de 2⁻¹² do passo.
- **`raycastVoxel`**: DDA de Amanatides & Woo — O(passos), sem branch extra, ignora água.
- **`simulateBodies`**: corpos de 64 B (1 por cache line), sono automático (1,2 s parado → sai do
  laço), empuxo na água, atrito por eixo. **0,12 µs por corpo.**

---

## 5. Formato do vértice (o detalhe que economiza banda)

16 bytes, 4 × `u32`, desempacotados **na GPU** com bit ops de 1 ciclo:

```
wordA : x (u16) | y (u16)
wordB : z (u16) | normal (u3 << 16) | AO (u2 << 19) | emissivo (u4 << 21)
wordC : u (u8) | v (u8) | camada do atlas (u16)
wordD : skyLight (u8) | blockLight (u8) | tint (u8) | reservado
```

Uma alternativa "natural" (posição float3 + normal float3 + uv2 + ao1) daria 32–40 B/vértice.
Aqui são 16 B: **30–50% menos banda de memória de vértice**, que é o gargalo real de mundos voxel.

O AO usa o padrão de 3 amostras por canto (side1, side2, diagonal) — 3 leituras de byte e um
ganho visual enorme no contato entre blocos.

---

## 6. Renderer (`src/render/renderer.ts`)

Cinco pipelines e nada mais:

| Pipeline | Estado | Uso |
|---|---|---|
| `sky` | triângulo gigante, `depthCompare: 'always'`, sem vertex buffer | céu com gradiente, sol, estrelas e haze |
| `chunk` | `cullMode: 'back'`, `frontFace: 'ccw'`, depth write | terreno opaco |
| `water` | blend `src-alpha`, `cullMode: 'none'`, sem depth write | água, vidro e gelo (1 segmento) |
| `bodies` | instancing de cubo (36 vértices × 4096 instâncias) | peças rígidas, 1 draw call |
| `outline` | `line-list`, blend | contorno do bloco sob a mira |

Decisões de custo:

- **Sem bind group por chunk.** O atlas é uma `texture_2d_array` global e a câmera é um uniform
  global, então cada chunk precisa apenas de `setVertexBuffer(buf, offset, size) + draw(count)`.
- **Sem transformação por chunk.** As posições dos vértices já são de mundo (u16 cobrem 0..65535,
  o mundo tem 384×128×384).
- **Empacotamento justo na VRAM**: `[opacos][translúcidos]` por chunk, com crescimento de 25%
  (evita realocar a cada remesh). Mundo inteiro ≈ 60–90 MB de VRAM.
- **Ordem dos desenhos**: opacos do mais perto ao mais longe (early-z perfeito), translúcidos do
  mais longe ao mais perto (blend correto).
- **Culling** por AABB contra 6 planos extraídos da view-projection + corte por distância.

---

## 7. Fluxo de arranque (o que a tela de carregamento realmente faz)

1. Checagens de ambiente (WebGPU, COOP/COEP, núcleos, SIMD por compilação de um módulo mínimo).
2. Cria a memória compartilhada — com **fallback** para memória comum e o binário
   `core-noshared.wasm` quando não há isolamento de origem.
3. Instancia o núcleo WASM (11 KB).
4. Cria 7 workers de chunk + 1 de física, entregando a `WebAssembly.Memory` por structured clone.
5. Gera o atlas procedural (18 camadas 32×32 em memória, zero download de asset).
6. Publica as 576 tarefas **em espiral a partir do spawn** → o mundo materializa do jogador para fora.
7. Ao completar, cria corpos rígidos, posiciona o jogador na superfície e troca para o jogo.

---

## 8. Decisões de engenharia (e o que foi descartado)

| Decisão | Alternativa descartada | Motivo |
|---|---|---|
| Saída não-indexada (6 vértices/quad) | index buffer por chunk | mais 2 vértices custam menos que gerenciar/realocar IBs com greedy meshing variável |
| Vértices u16 absolutos | floats + uniform por chunk | elimina bind group e multiplicação por vértice |
| 1 byte por voxel | paleta/bit-packing | 32 KB/chunk cabe em cache; decode custaria ciclos no caminho quente |
| Mesher no WASM | mesher em JS | 5–8× mais rápido pela ausência de GC e pelo SIMD real |
| `--runtime stub` | runtime incremental (GC) | nenhuma alocação em runtime = nenhum jitter |
| Física do jogador na main thread | worker dedicado | 5 µs não justifica latência de mensagem; corpos rígidos vão para worker |
| Atlas procedural | PNG de assets | abertura instantânea, zero rede, camadas perfeitamente alinhadas |
| Tailwind puro na HUD | framework reativo | zero reconciliação por frame; atualização cirúrgica a 5 Hz |

---

## 9. Onde estender

- **Mundo maior/streaming**: `TABLE` já suporta slots pendentes (valor `<-1`) e o pool pode passar
  a reciclar slots; basta um policy de "carregar anel em volta do jogador".
- **Luz de bloco**: o campo `blockLight` do vértice já existe e não é usado; um BFS de luz em worker
  escreveria num segundo buffer por chunk.
- **Multiplayer**: todo o estado é bytes contíguos — diffs de chunk são triviais de comprimir.
- **Novos blocos**: adicione o ID em `assembly/constants.ts`, a camada no atlas
  (`src/render/atlas.ts`) e a cor da hotbar (`src/game/engine.ts`).
