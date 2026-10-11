# Desempenho — metodologia, números e diário de bugs

## 1. Como medir

```bash
npm run test:core     # benchmarks do núcleo (Node, 1 núcleo, sem GPU)
npm run bench         # idem
```

Dentro do jogo, `F3` mostra telemetria ao vivo:

| Métrica | O que significa | Alvo |
|---|---|---|
| `frame` | ms por frame (limitado pelo vsync a 16,7 ms) | ≤ 16,7 |
| `cpu` | **todo** o trabalho da main thread no frame | < 3 ms |
| `draw calls` | chamadas de desenho no passe | < 120 |
| `tri` | triângulos submetidos | < 500k |
| `chunks vis/cortados` | eficácia do frustum + corte por distância | ≥ 60% cortados |
| `física` | ms gastos no *dispatch* da física (não na simulação) | < 0,3 ms |

## 2. Números medidos

### Núcleo (Node 22, 1 thread, sem GPU)

| Operação | Resultado | Observação |
|---|---|---|
| `generateChunk` (32³ blocos) | **0,90 ms/chunk** | heightmap + cavernas + árvores |
| `meshChunk` (greedy + AO) | **0,69 ms/chunk** | média de 188 quads/chunk |
| Mundo completo (576 chunks) | **917 ms** single-thread | 518 ms geração + 395 ms meshing |
| Mundo completo projetado | **~115 ms com 7 workers** | escala quase linear (trabalho independente) |
| `simulateBodies` | **0,125 ms/frame** para 1.000 corpos | 0,12 µs por corpo/tick |
| Fila de tarefas | **0,20 ms** para 576 push + claim | sem lock, sem cópia |
| Vértices do mundo | **649k** (108k quads) | vs. ~1,4 M do método 1-quad-por-voxel |

### Orçamento de memória

| Recurso | Uso | Limite |
|---|---|---|
| Heap compartilhado | 28,0 MB | 32 MB (512 páginas) |
| Voxels | 20,0 MB | 640 chunks × 32 KB |
| Saída do mesher (rascunho) | 5,0 MB | 8 workers × 640 KB |
| Arenas de rascunho | 512 KB | 8 × 64 KB |
| Corpos rígidos | 256 KB | 4096 × 64 B |
| VRAM (meshes) | ~60–90 MB | empacotamento justo por chunk |
| VRAM (atlas) | 73,7 KB | 18 camadas 32×32 RGBA8 |
| **Download total** | **~40 KB gzip** | JS 65 KB + WASM 11 KB + CSS 16 KB |

### Por que 60 FPS estáveis

O frame tem 16,7 ms. A main thread consome 0,6–1,5 ms; o resto é GPU (passe de geometria de
~500k triângulos, que é um alvo trivial para qualquer GPU de 2015+). Como as posições dos
vértices são indexadas por `u16` absolutos e há pouquíssimos draw calls (≈1 por chunk visível,
com apenas 2 por chunk quando há água), o gargalo real passa a ser **banda de memória**, não
geometria — e é exatamente isso que o formato de 16 B/vértice e o greedy meshing atacam.

---

## 3. Diário de bugs reais (encontrados e corrigidos no desenvolvimento)

Estes não são "bugs de digitação": são armadilhas estruturais de engines voxel, e cada uma
custaria um dia de depuração em produção.

### 3.1 `fillBytes` de coluna (crítico, silencioso)

**Sintoma:** o mundo gerado mostrava linhas longas de pedra cravadas no céu, e o terreno
sumia em fatias de 32 blocos.

**Causa:** o layout usa `idx = x | (z << 5) | (y << 10)` — o eixo Y tem `stride` 1024.
`fillBytes(ptr, BLK_STONE, N)` preenche **N bytes contíguos**, ou seja, ao longo de **X**,
não de Y. Preencher uma coluna com `fillBytes` corrompia 32 linhas inteiras de outros planos.

**Correção:** escrita escalar com `col + (yl << 10)` para o eixo Y (e SIMD reservado para
varreduras contíguas — a limpeza do chunk e o `i8x16` de varredura de plano, que são
contíguas de verdade).

### 3.2 Coordenada global vs. local no gerador (crítico, silencioso)

**Sintoma:** chunks em `cy > 0` viravam cópias de pedra, sem relação com o relevo.

**Causa:** a altura do terreno é **global** (`h ≈ 30`), mas o índice do voxel é **local**
(`0..31` dentro do chunk). Usar `h` direto como índice local escrevia fora do slice correto.

**Correção:** conversão única `hl = h − (cy << 5)` por coluna, com poda precoce
(`if (hl < 0 && seaLocal < 0) continue`) para não gastar tempo em slices de céu.

### 3.3 Tunneling na colisão (crítico)

**Sintoma:** um corpo caindo a 60 blocos/s atravessava o chão; a solução "snap no voxel"
empurrava corpos para dentro de paredes.

**Correção:** bissecção de 12 iterações por eixo (precisão 2⁻¹² do passo) — nunca há
penetração nem tunneling, com custo de ~12 testes de AABB apenas quando há colisão.

### 3.4 Memória compartilhada não linkava (crítico, só no navegador)

**Sintoma:** `TypeError: mismatch in shared state of memory, declared = 0, imported = 1` — tela
fatal no primeiro clique.

**Causa:** o script de build de desenvolvimento compilava sem `--sharedMemory`, enquanto o JS
cria uma memória `shared: true`.

**Correção:** flags canônicas em `npm run wasm` + validação automática `tools/wasm-info.mjs`
que **linka os dois binários com o tipo de memória correspondente durante o build**.
Regressão coberta para sempre.

### 3.5 Buffer de GPU por worker em vez de por chunk

**Sintoma:** com 576 chunks e 8 workers, os chunks se sobrescreviam na VRAM (você via o mundo
"piscando" em pedaços).

**Causa:** o worker é uma **linha de montagem**, não um destino. Um buffer por worker significa
que o 2º chunk processado pelo mesmo worker destrói o mesh do 1º.

**Correção:** buffer por **slot de chunk**, com crescimento de 25% e empacotamento
`[opacos][translúcidos]` por chunk.

### 3.6 300 MB de VRAM por acidente

**Sintoma:** consumo absurdo de VRAM ao carregar.

**Causa:** cada buffer de GPU era alocado com o tamanho do *rascunho* (640 KB) em vez do
tamanho do *mesh* (~80 KB).

**Correção:** alocação pelo tamanho real + histerese de crescimento; ajuste do deslocamento do
segmento translúcido para o valor empacotado (`waterOffset` dinâmico).

### 3.7 `Atomics.wait` na main thread

**Sintoma:** exceção ao construir a fila sem multithreading.

**Causa:** `Atomics.wait` é proibido na main thread do navegador.

**Correção:** o anel de tarefas só é criado no modo multithread; o caminho single-thread usa
um `Array` de tarefas com fatiamento de 6 ms por frame (a UI nunca trava).

---

## 4. O que ainda dá para ganhar (com honestidade)

| Ganho | Ideia | Custo de implementação |
|---|---|---|
| 20–40% | **LOD por distância** (chunks distantes com passo 2×2 → 1/4 dos quads) | médio — segundo caminho no mesher |
| 10–25% | **Culling de faces por oclusão** em vez de só frustum | médio |
| 5–15% | **Compactar vértice para 8 B** (posição relativa ao chunk + wavefront) | baixo/médio |
| 2–5× (I/O) | **Delta de chunks para rede** (multiplayer) | alto (depende de servidor) |
| 10–30% | **Indirect draw + culling na GPU** (compute) | alto |
| — | **Luz propagada** (`blockLight` já reservado no vértice) | médio — qualidade visual |

Nenhuma dessas é necessária para 60 FPS no mundo atual: a folga de CPU é de ~15 ms por frame.
