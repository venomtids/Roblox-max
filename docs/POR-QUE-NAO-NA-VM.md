# Por que o Roblox não roda dentro de um "Linux no navegador"

Este documento responde, com precisão técnica, a pergunta:

> *"Dá para instalar um Linux dentro do site e instalar o Roblox nele?"*

**Resposta curta:** *Linux no navegador* — sim, é real e funciona (v86, WebVM/CheerpX, anuraOS).
*Roblox dentro dele* — não, e não é questão de esforço: são **quatro impedimentos independentes**,
e qualquer um deles basta para bloquear. Esta é a análise honesta de cada um.

---

## 1. Arquitetura: 32 bits × x86-64 (impedimento absoluto)

| | |
|---|---|
| Emulador de PC em WebAssembly mais maduro | **v86** — emula um PC de **32 bits** (i386, nível Pentium, sem "long mode") |
| Cliente do Roblox que existe para Linux | **APK Android x86-64** (via Sober) |
| Resultado | O código é compilado para **x86-64**: ele emite instruções de 64 bits que **não existem** no emulador |

Não é lentidão — é a instrução não existir. Um emulador de 64 bits em WASM *pode* ser escrito
(o QEMU tem builds experimentais em WASM), mas aí os impedimentos 2, 3 e 4 continuam intactos.

> Nota justa sobre o **WebVM/CheerpX**: ele faz virtualização de *userland* x86-64 no navegador
> (sem kernel), o que é impressionante. Mas: (a) é proprietário, com licença que não permite uso
> comercial/auto-hospedado livre; (b) não há GPU; (c) um jogo 3D precisa de kernel (syscalls
> gráficas, threads, scheduler), não de userland isolado.

---

## 2. GPU: não existe Vulkan nem OpenGL ES dentro de uma VM de navegador

Um emulador full-system expõe **VGA emulado** (um framebuffer escrito por software) e, no máximo,
VirtIO-GPU emulada. Não existe hoje — e não há mecanismo especificado — para passar **WebGPU/WebGL
do navegador para dentro da VM** como driver nativo.

E o runtime que executa o Roblox no Linux exige, por requisito mínimo declarado:

- **Vulkan 1.0+** (recomendado 1.1) **ou** OpenGL ES 3.0+, e
- um driver de GPU real com aceleração de hardware.

Sem driver gráfico, o cliente do Roblox **nem inicializa** — a falha acontece antes de qualquer
tela de login.

---

## 3. Anti-cheat e Termos de Uso (impedimento de política, não de código)

- A Roblox **bloqueou Wine de propósito** (atualização de 27/02/2024) porque ambientes de emulação
  estavam sendo usados para trapaça. O anti-cheat **Hyperion** tem detecção de ambiente que faz
  parte do produto.
- Rodar o cliente dentro de uma **máquina virtual no navegador** pertence exatamente à mesma
  categoria de ambiente emulado: seria detectado/recusado pelo produto, e a conta pode ser banida
  pelos **Termos de Uso**.
- Por isso este projeto **não constrói nem incentiva** caminho de contorno: seria entregar a você
  uma ferramenta que derruba sua conta.
- O caminho que **funciona e é aceito na prática** é o **Sober** — e ele funciona porque executa o
  **APK oficial do Roblox para Android**, com verificação de assinatura, num runtime que traduz
  chamadas do Android para o kernel Linux real. Não é emulação de CPU, e não é contorno de
  anti-cheat: o anti-cheat do APK continua ativo e intacto.

---

## 4. Desempenho: 1–5% do nativo (impedimento prático)

Um emulador de sistema completo em WebAssembly (JIT de x86 → WASM) entrega, na melhor das
hipóteses, algo entre **1% e 5%** do desempenho nativo. Para referência:

| Cenário | Ordem de grandeza |
|---|---|
| Shell/console em VM de navegador | utilizável (~segundos de espera) |
| Compilar algo dentro dela | minutos por linha de código |
| Jogo 3D com física em tempo real, 60 FPS | **impossível** (menos de 1 FPS) |

O Sober, para comparação, roda a **~93% do desempenho do cliente Windows nativo** — porque ele não
emula CPU: ele é um runtime de tradução (estilo "container" do Android) rodando sobre o kernel
Linux real, com GPU real.

---

## 5. Resumo em uma tabela

| Caminho | Linux roda? | Roblox roda? | GPU real? | Respeita ToS? |
|---|---|---|---|---|
| **Sober (Flatpak) em Linux real** | — | **✅ Sim** | ✅ Vulkan | ⚠️ Não endossado, sem bans reportados |
| Roblox nativo em Windows/macOS | — | ✅ Sim | ✅ | ✅ Oficial |
| Cliente Android oficial | — | ✅ Sim | ✅ | ✅ Oficial |
| Wine/Proton (2026) | — | ❌ Bloqueado pelo Hyperion | ✅ | ❌ |
| **Linux (v86) dentro do navegador** | ✅ (32-bit) | ❌ 32 vs 64 bits | ❌ VGA emulado | ❌ |
| Linux (WebVM/CheerpX) no navegador | ✅ (userland) | ❌ sem kernel/GPU + licença | ❌ | ❌ |
| **Este projeto (motor próprio no navegador)** | — | ➖ o *nosso* jogo roda | ✅ WebGPU | ✅ (é nosso código) |

---

## 6. O que fazer então (caminhos reais, em ordem de atrito)

1. **Você quer jogar Roblox de verdade, hoje, com o mínimo de atrito?**
   - **Windows/macOS:** baixe o instalador oficial em <https://www.roblox.com/download>. Dois minutos, caminho suportado.
   - **Linux:** rode `roblox-linux.sh install` (Sober). Guia: [`roblox-linux/README.md`](../roblox-linux/README.md).
   - **Steam Deck:** Modo Desktop para instalar, Modo Jogo para jogar.
   - **Android/iOS/ChromeOS:** app oficial da loja.
   A **Central de Instalação** dentro do site detecta seu sistema e mostra esse passo a passo.

2. **Você quer jogar algo 3D no navegador sem instalar nada?**
   Use o motor deste repositório (arquivo único `roblox-max.html`, duplo clique) — WebGPU + WASM,
   com edição de blocos, física e voo. Não é o Roblox, é *seu* motor — e é honesto.

3. **Você quer criar e publicar seus próprios jogos?**
   Instale o **Roblox Studio** no Linux via **Vinegar** (`./roblox-linux.sh studio`) ou direto no
   Windows/macOS. Studio não tem Hyperion, por isso funciona em Linux.
