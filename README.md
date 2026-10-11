# ROBLOX-MAX

**Plataforma de jogos 3D estilo Roblox que roda 100% dentro do navegador** — sem plugin, sem instalação, sem download de assets.
Renderização de baixo nível em **WebGPU**, simulação e malhas geradas em **WebAssembly com SIMD**, e paralelismo real de CPU com **Web Workers + SharedArrayBuffer** (o mesmo heap do WASM é compartilhado entre todas as threads).

```
WebGPU (3 pipelines + MSAA 4×)      ← render
WASM SIMD (AssemblyScript, 11 KB)   ← geração, meshing, física, colisão
Web Workers + SharedArrayBuffer     ← 8 threads no MESMO linear memory
Tailwind CSS                        ← HUD gamer responsiva
```

---

## ⚡ Rodar agora

### Opção A — pelo preview desta sessão (nada para instalar)
O servidor já está no ar (Vite, porta 5173) com os headers `COOP/COEP` necessários para multithreading. Abra o preview, espere a barra de progresso (≈1–2 s) e clique em **▶ JOGAR**.

### Opção B — um arquivo só, duplo clique (não precisa de Node!)
Baixe **`roblox-max.html`** (117 KB) direto do link na tela inicial, ou de
[`public/downloads/roblox-max.html`](public/downloads/roblox-max.html), ou pelo preview
em `/downloads/roblox-max.html`. Dê **duplo clique** no arquivo: o jogo abre no navegador
sem servidor, sem instalação, sem internet.

Essa versão é 100% autocontida (WASM embutido em `data:` base64, CSS inline, bundle IIFE —
nenhum request de rede). Em `file://` o navegador não permite `SharedArrayBuffer`, então ela
usa o núcleo `core-noshared.wasm` e gera o mundo na main thread (fatiado por frame): a
abertura leva ~2–4 s em vez de ~1 s. **Para multithreading real, use a versão web** (Opção A/C).

Gerar o seu próprio arquivo único:

```bash
npm run build:standalone   # → public/downloads/roblox-max.html
```

### Opção C — na sua máquina (desenvolvimento)
Você **não precisa** de Node para *jogar*: o site é estático. Mas para *buildar/rodar localmente*:

```bash
# 1. Instale o Node.js 20+ (https://nodejs.org) — única dependência de sistema
# 2. Clone e instale
git clone https://github.com/venomtids/Roblox-max.git
cd Roblox-max
npm install

# 3. Compile o núcleo WASM + suba o servidor de desenvolvimento
npm start            # → http://localhost:5173

# Alternativas
npm run build        # build de produção em dist/ (sirva com COOP/COEP)
npm run preview      # serve o build de produção
npm run verify       # wasm + typecheck + 22 testes do núcleo + build
npm run test:core         # só o harness de validação (Node puro, sem navegador)
npm run build:standalone  # gera o arquivo único autônomo (117 KB)
```

> ⚠️ **Servidor sem COOP/COEP?** Sem os headers `Cross-Origin-Opener-Policy: same-origin` e
> `Cross-Origin-Embedder-Policy: require-corp` o navegador desliga o `SharedArrayBuffer`.
> O jogo **continua funcionando** (cai automaticamente para o núcleo `core-noshared.wasm`
> e gera os chunks na main thread), apenas sem os 8 workers.

---

## 🎮 Controles

| Ação | Tecla |
|---|---|
| Mover | `W A S D` |
| Pular (com coyote-time + pulo variável) | `Espaço` |
| Correr / Agachar | `Shift` / `Ctrl` |
| Voar (modo criativo) | `V` |
| Quebrar bloco | Clique **esquerdo** |
| Colocar bloco | Clique **direito** |
| Selecionar bloco | `1`–`9` ou scroll |
| Soltar uma peça física (estilo *part*) | `G` |
| Explosão radial (acorda e arremessa peças) | `E` |
| Respawn | `R` |
| Ciclo dia/noite rápido | `N` |
| Telemetria (FPS, ms, triângulos, chunks) | `F3` |
| Menu de configurações (render scale, FOV, peças…) | `ESC` |

**Mobile:** metade esquerda da tela = analógico de movimento; metade direita = olhar.

---

## 🏗️ Arquitetura em 30 segundos

```
                    ┌────────────────────────── MAIN THREAD ──────────────────────────┐
                    │  input → Player.step() → raycast → edições → render → HUD        │
                    └───────┬──────────────────────────┬──────────────────────┬────────┘
                            │ postMessage(Memory)      │ SAB (views)          │ draw()
                            ▼                          ▼                      ▼
        ┌───────────────────────────────┐  ┌──────────────────────┐  ┌────────────────┐
        │  7 × WORKER DE CHUNK          │  │  WORKER DE FÍSICA    │  │  GPU (WebGPU)  │
        │  generateChunk() → meshChunk()│  │  simulateBodies()    │  │  pipelines     │
        └───────────────┬───────────────┘  └──────────┬───────────┘  └────────────────┘
                        │                             │
                        └────────►  32 MB SharedArrayBuffer  ◄─── VOCÊS/VOXELS/MESHOUT/ARENAS
                                    (o MESMO linear memory do WASM)
```

| Camada | Arquivo | Papel |
|---|---|---|
| Núcleo WASM | `assembly/*.ts` | ruído procedural, gerador de mundo, **greedy mesher SIMD**, sweep AABB, DDA raycast, simulação de corpos |
| Contrato binário | `src/core/memory.ts` | mapa de memória compartilhada (header, tabela de chunks, anel de tarefas, arenas, mesh out, voxels) |
| Fila lock-free | `src/world/tasks.ts` | anel SPSC/MPMC com `Atomics` — zero locks, zero alocação |
| Coordenação | `src/world/chunkPool.ts` | cria workers, publica tarefas, consome meshes direto da memória |
| Render | `src/render/renderer.ts` + `shaders.ts` | 3 pipelines, vértice de 16 B empacotado, frustum culling, MSAA |
| Jogo | `src/game/*` | jogador, física no WASM, edição de blocos, peças rígidas |
| UI | `src/ui/hud.ts` | HUD gamer em Tailwind, atualizada cirurgicamente a 5 Hz |
| Validação | `tools/harness.ts` | 22 testes do núcleo rodando em Node puro |

Detalhes profundos: **[docs/ARQUITETURA.md](docs/ARQUITETURA.md)** · **[docs/WASM-API.md](docs/WASM-API.md)** · **[docs/DESEMPENHO.md](docs/DESEMPENHO.md)**

---

## 📊 Números medidos (não estimados)

`npm run test:core` — Node 22, 1 núcleo, sem GPU:

| Operação | Medido | Projeção em jogo |
|---|---|---|
| Geração de mundo (`generateChunk`, 32³) | **0,90 ms/chunk** | 576 chunks em ~518 ms ÷ 7 workers ≈ **74 ms** |
| Greedy meshing (`meshChunk`) | **0,69 ms/chunk** | 108.136 quads / 649k vértices no mundo inteiro |
| Compressão do mesher vs. 1 quad/voxel | **−90% a −97%** | ~1,4 M vértices → 649k |
| Física (`simulateBodies`) | **0,12 µs por corpo** | 1.000 peças a 60 Hz = **0,125 ms/frame** |
| Fila de tarefas (576 tarefas + consumo) | **0,20 ms** | distribuição em 7 threads sem lock |
| Mundo completo (gerar + meshar, 1 thread) | **917 ms** | **~115 ms com 7 workers** |
| Tamanho do engine | **65 KB JS + 11 KB WASM + 16 KB CSS** | ~40 KB gzip total |

E a lista completa de bugs reais encontrados e corrigidos durante o desenvolvimento
(dois deles silenciosos e devastadores) está em [docs/DESEMPENHO.md](docs/DESEMPENHO.md#-diário-de-bugs-reais).

---

## 🔧 Requisitos de navegador

| Recurso | Mínimo |
|---|---|
| **WebGPU** | Chrome/Edge 113+, Chrome Android 121+, Firefox 141+ (com flags), Safari 26+ |
| **WASM SIMD** | Chrome 91+, Firefox 89+, Safari 16.4+ |
| **Multithreading (SAB)** | exige `crossOriginIsolated` (COOP+COEP) — o servidor já configura |
| **Sem WebGPU?** | a tela inicial explica exatamente o que habilitar (não é uma tela branca) |

---

## 🧭 Roadmap natural (o que falta para ser 100% Roblox)

O que já existe: mundo voxel 384×128×384, física rígida, edição de blocos, peças estilo *part*, ciclo dia/noite, voo criativo, HUD completa, multithreading real.

O que um clone completo do Roblox ainda pediria (em ordem de valor):
1. **Multiplayer** — `WebRTC`/`WebTransport` + autoridade do servidor; o estado já é "só bytes", então delta-compressão de voxels é natural.
2. **Light propagation** por voxel (BFS de luz em worker) — o atributo `blockLight` do vértice já está reservado e não usado.
3. **Streaming/LOD** — mundo infinito com anel de chunks e uso da tabela `slot pendente` já prevista no mapa de memória.
4. **Editor de scripts** — Lua/WASM guest de terceiros em sandbox de worker, com o próprio mesher como API de mundo.
5. **Modelos e ferramentas de criação** — importar mesh glTF, rigging simples, animação por keyframes na GPU.

---

## 🐧 Roblox de verdade no Linux

Este projeto é a **plataforma no navegador**. Para rodar o **Roblox oficial** no Linux
existe um companheiro neste repositório que instala o caminho que funciona em 2026
(o runtime **Sober**, que executa o APK oficial do Roblox para Android):

```bash
./roblox-linux/roblox-linux.sh doctor    # diz o que falta no seu sistema
./roblox-linux/roblox-linux.sh install   # Flatpak + Flathub + drivers + Sober
```

Guia completo, notas por distro e tabela de problemas: **[roblox-linux/README.md](roblox-linux/README.md)**
Também disponível para download direto em `/downloads/roblox-linux.sh`.

> Importante: **Wine/Proton/Vinegar estão mortos** para o Roblox Player desde 27/02/2024
> (o anti-cheat Hyperion é Windows-only e não roda sob Wine). O caminho Android via Sober
> é o único que funciona — e não há cliente oficial para Linux.

## 📄 Licença

MIT — use, estude, publique. Feito com foco absoluto em performance de navegador.
