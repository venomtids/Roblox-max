# Contrato binário WASM ↔ JavaScript

Este documento é a **fonte da verdade** da interface entre o núcleo AssemblyScript e o TypeScript.
Se algo aqui mudar, `src/core/memory.ts`, `src/world/jobs.ts` e o harness precisam mudar junto —
`coreVersion()` (hoje `10007`) existe justamente para o JS recusar um binário incompatível.

---

## 1. Build

```bash
# multithreading (padrão): memória compartilhada + threads
asc assembly/index.ts --optimize --shrinkLevel 1 --noAssert \
    --enable simd --enable bulk-memory --enable threads \
    --importMemory --sharedMemory --maximumMemory 512 \
    --runtime stub --exportRuntime \
    --outFile public/wasm/core.wasm

# compatibilidade: sem shared memory (ambientes sem COOP/COEP)
asc assembly/index.ts --optimize --shrinkLevel 1 --noAssert \
    --enable simd --enable bulk-memory \
    --importMemory --maximumMemory 512 \
    --runtime stub --exportRuntime \
    --outFile public/wasm/core-noshared.wasm
```

> Os dois binários **precisam** existir. O import de memória de um módulo compilado com
> `--sharedMemory` exige uma memória `shared: true`; sem isolamento de origem o navegador
> nem deixa criar `SharedArrayBuffer`. `tools/wasm-info.mjs` valida esse pareamento no build.

## 2. Imports esperados pelo módulo

| Módulo | Nome | Tipo | Obrigatório |
|---|---|---|---|
| `env` | `memory` | `WebAssembly.Memory` | sim — a memória do jogo, criada no JS |
| `env` | `abort` | `(msg, file, line, col) => void` | sim — deve lançar erro para não corromper o heap |

## 3. Exports

| Export | Assinatura | Semântica |
|---|---|---|
| `coreVersion()` | `i32` | versão do contrato binário (validada no load) |
| `generateChunk(hdr, slot, cx, cy, cz)` | `void` | gera o chunk 32³ por inteiro (determinístico) |
| `meshChunk(hdr, slot, cx, cy, cz, outPtr, cap, scratch)` | `i32` | greedy meshing; escreve cabeçalho + vértices em `outPtr`; devolve nº de vértices |
| `getBlockW(hdr, x, y, z)` | `i32` | voxel em coordenada de mundo (`1` = sólido virtual fora do mundo) |
| `setBlockW(hdr, x, y, z, id)` | `void` | escrita de voxel |
| `isOpaque(id)` / `isSolidForPhysics(id)` | `i32` | classificação de bloco |
| `fillBytes(ptr, pattern, count)` | `void` | preenchimento SIMD (16 B/instrução) |
| `copyBytes(dst, src, count)` | `void` | memcpy SIMD |
| `sweepBox(hdr, px,py,pz, dx,dy,dz, hx,hy,hz, outPtr)` | `i32` | colisão AABB com bissecção; devolve flags |
| `raycastVoxel(hdr, ox,oy,oz, dx,dy,dz, maxDist, outPtr)` | `i32` | DDA; devolve id do bloco atingido (0 = nada) |
| `surfaceY(hdr, x, z)` | `i32` | primeira superfície sólida descendo do topo |
| `isSpawnClear(hdr, x, y, z)` | `i32` | há 3 blocos livres a partir de `y`? |
| `simulateBodies(hdr, bodyBase, count, dt, scratch)` | `i32` | integra `count` corpos; devolve bit0 = algum acordado |
| `wakeBodies(bodyBase, count, x, y, z, radius)` | `void` | acorda corpos no raio |
| `warmup(hdr, scratch)` | `i32` | aquece caches/funções na inicialização |

### Flags de `sweepBox`

| Bit | Constante | Significado |
|---|---|---|
| 1 | `COL_GROUND` | colidiu descendo (pés no chão) |
| 2 | `COL_WATER` | corpo imerso (empuxo) |
| 4 / 8 / 16 | `COL_X` / `COL_Y` / `COL_Z` | colisão naquele eixo |
| 32 | `COL_CEIL` | colidiu subindo (teto) |

### Layout de saída (`outPtr`)

| Função | Bytes |
|---|---|
| `meshChunk` | `[0] u32` vértices opacos · `[4] u32` vértices translúcidos · `[8] u32` truncou? · `[12] u32` quads · `[16..40] f32×6` AABB mundo · `[40] u32` slot do chunk · `[64..]` vértices |
| `sweepBox` | `[0..12] f32` posição final · `[12] i32` flags |
| `raycastVoxel` | `[0..12] f32` bloco atingido · `[12] i32` id · `[16..28] i32` normal · `[28] f32` distância |

### Vértice (16 B)

```
wordA : x (u16) | y (u16)
wordB : z (u16) | normal (u3 << 16) | ao (u2 << 19) | emissivo (u4 << 21)
wordC : u (u8) | v (u8) | camada (u16)
wordD : skyLight (u8) | blockLight (u8) | tint (u8) | reservado
```

## 4. Mapa de memória

Ver [ARQUITETURA.md §2](ARQUITETURA.md#2-memória-compartilhada-o-barramento-do-engine).
Resumo dos ponteiros usados pelas chamadas:

| Parâmetro | Valor (de `LAYOUT`) |
|---|---|
| `hdr` | `0x001000` |
| `bodyBase` | `0x020000` |
| `scratch` (worker `i`) | `0x060000 + i × 0x10000` |
| `outPtr` (worker `i`) | `0x100000 + i × 0xA0000` |
| chunk `slot` `s` | `0x800000 + s × 32768` |

## 5. Protocolo de threads (Atomics)

| Índice em `CONTROL` | Nome | Uso |
|---|---|---|
| `0` | `TASK_HEAD` | total de tarefas publicadas (`Atomics.add` + `notify`) |
| `1` | `TASK_CLAIM` | próximo índice a reservar (`Atomics.add`) |
| `2` | `TASK_DONE` | concluídas (`Atomics.add`) |
| `3..6` | `EDIT_EPOCH/X/Y/Z` | edição do mundo (acorda corpos; X/Y/Z em fixed point × 32) |
| `7` | `PHYS_TICK` | pedido de passo de física (`Atomics.add` + `notify`) |
| `8` | `PHYS_DONE` | confirmação do worker de física |
| `9` | `AWAKE` | algum corpo acordado (bit0) |
| `10` | `PHYS_DT` | `f32` do passo (escrito antes de cada tick) |
| `16 + i*2` | `MESH_SEQ[i]` | sequência de mesh publicada pelo worker `i` |
| `16 + i*2+1` | `MESH_ACK[i]` | confirmação de consumo da main thread |

Anel de tarefas em `0x008000`: entrada `(i % 4096) × 4` i32 = `[kind, cx, cy, cz]`, com
`kind = 0` (gerar + meshar) ou `kind = 1` (só remeshar após edição).

## 6. Como testar sem navegador

```bash
npm run test:core   # 22 asserções: link da memória, anel de tarefas, integridade
                    # dos vértices, colisão, raycast, física, uso de memória
```

O harness instancia o `.wasm` real sobre uma `WebAssembly.Memory` compartilhada e percorre
todo o caminho de dados — só a camada WebGPU fica de fora (impossível fora do navegador).
