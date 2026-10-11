# ROBLOX-LINUX — Roblox no Linux, do jeito que funciona em 2026

Ferramenta de terminal que **instala, diagnostica e mantém** o Roblox no Linux pelo único caminho que funciona hoje: o **Sober**.

```
./roblox-linux.sh            # menu interativo
./roblox-linux.sh doctor     # diagnóstico completo do seu sistema
./roblox-linux.sh install    # instala tudo (flatpak + flathub + drivers + Sober)
```

---

## ⚠️ Leia isto antes (honestidade antes de tudo)

| | |
|---|---|
| **A Roblox publica cliente para Linux?** | **Não.** Windows, macOS, Android, iOS e Xbox. Não existe `.deb`, `.rpm` nem Flatpak oficial. |
| **Então o que isto instala?** | O **Sober** — runtime comunitário do time VinegarHQ que executa o **APK oficial do Roblox para Android**, baixado dos servidores oficiais e com verificação de assinatura rigorosa. |
| **Por que Wine não serve?** | Desde **27/02/2024** o anti-cheat **Hyperion** bloqueia o cliente Windows sob Wine/Proton de propósito. **Vinegar, Grapejuice, Lutris, Heroic: todos mortos para o Player.** Qualquer tutorial que os recomende está desatualizado. |
| **E o Hyperion não bloqueia o Sober?** | O Hyperion é um driver de kernel **Windows**. Ele não existe no caminho Android. O APK tem o anti-cheat próprio dele, intacto. |
| **Tem risco de ban?** | O ToS da Roblox não cita o Sober explicitamente. Até **05/2026** não há ondas de banimento reportadas pelo seu uso, e o servidor te enxerga como jogador Android comum. Existe 1 relato isolado e sem causa confirmada (06/2025). **A decisão é sua** — e esteja ciente de que flags/modificações do cliente (FastFlags) *aumentam* esse risco. |
| **O que este software NÃO faz** | Não cria, altera, decompila ou falsifica cliente. Não contorna anti-cheat. Não conversa com os servidores da Roblox. Não baixa binário de terceiros — instala via Flatpak do Flathub, com sandbox. |

> Quem joga é o **cliente oficial da Roblox**, rodando dentro do runtime do Sober. Este instalador só automatiza a instalação e diagnostica o seu sistema.

---

## Requisitos (o script confere todos e te diz o que corrigir)

| Item | Mínimo | Recomendado |
|---|---|---|
| CPU | **x86-64 com SSE4.1** | SSE4.2 (sem ele há emulação — FPS bem menor) |
| Kernel | Linux **5.11+** | qualquer kernel atual (6.x) |
| GPU | **OpenGL ES 3.0** | **Vulkan 1.1** (bem mais rápido) |
| Flatpak | obrigatório (é o canal do Sober) | — |
| Espaço | ~1,5 GB | 4 GB livres |
| RAM | 4 GB | 8 GB+ |
| Distros testadas | Ubuntu 24.04, Debian 12, Fedora 40, Arch, Mint 21+, Bazzite, SteamOS 3.5 | — |

**Não funciona em:** ARM (Raspberry Pi, Snapdragon), CPUs anteriores a ~2011, WSL2 (sem Vulkan), e dentro de container Docker.

---

## Como usar

### 1. Baixar o script
Baixe `roblox-linux.sh` (ou clone o repositório). Nada de `curl | bash`: o arquivo é legível, comentado e você pode auditar tudo antes.

### 2. Rodar o diagnóstico (não altera nada)
```bash
chmod +x roblox-linux.sh
./roblox-linux.sh doctor
```
Você recebe um relatório item por item, com **a correção exata** de cada problema — inclusive o comando do seu gerenciador de pacotes.

### 3. Simular a instalação (opcional, mas recomendado)
```bash
./roblox-linux.sh --dry-run install
```
Mostra cada comando que seria executado. **Nada é alterado.**

### 4. Instalar
```bash
./roblox-linux.sh install
```
Ele: instala o Flatpak se faltar → configura o Flathub → instala os drivers Vulkan/GLES da sua GPU → baixa e instala o Sober → cria o atalho no menu de aplicativos.

### 5. Primeira execução
Abra **“Roblox”** no menu de aplicativos (ou `flatpak run org.vinegarhq.Sober`):
1. Escolha **Automatic** (baixa o APK oficial — ~400 MB);
2. Faça login na sua conta Roblox;
3. Jogue.

---

## Notas por distro

| Distro | Observação |
|---|---|
| **Ubuntu / Debian / Mint / Pop!_OS** | `sudo apt install flatpak` e adicione o Flathub. Em Debian, habilite `flatpak` no repositório backports se a versão estiver velha. |
| **Fedora / Nobara** | `sudo dnf install flatpak`, Flathub pelo guia oficial (já vem habilitado no Nobara). |
| **Arch / Manjaro / Endeavour / Garuda / CachyOS** | `sudo pacman -S flatpak` + Flathub. Não instale o Sober pelo AUR: use o Flatpak (é o canal suportado). |
| **openSUSE** | `zypper install flatpak`, depois Flathub. |
| **SteamOS (Steam Deck)** | O Flatpak já vem pronto. Vá para o **Modo Desktop**, instale o Sober e crie o atalho; no Modo Jogo use "Adicionar jogo não-Steam" apontando para o Sober. |
| **Bazzite / Silverblue / Kinoite** | Sistema **imutável**: o script detecta e usa apenas Flatpak no escopo do usuário (sem sudo, sem mexer no sistema). |
| **NixOS** | Use `services.flatpak.enable = true;` e então este script. |

---

## Comandos

| Comando | O que faz |
|---|---|
| `./roblox-linux.sh` | Menu interativo |
| `./roblox-linux.sh doctor` | Diagnóstico do sistema com a correção de cada item |
| `./roblox-linux.sh install` | Instala Flatpak + Flathub + drivers + Sober + atalho |
| `./roblox-linux.sh launch` | Abre o Roblox |
| `./roblox-linux.sh update` | Atualiza o Sober (o Roblox muda toda semana — use quando parar de abrir) |
| `./roblox-linux.sh studio` | Instala o **Roblox Studio** via Vinegar (criar e publicar jogos) |
| `./roblox-linux.sh fastflags` | Abre os ajustes avançados do cliente (com aviso de risco + backup automático) |
| `./roblox-linux.sh uninstall` | Remove o Sober e os dados locais |

**Opções:** `-y/--yes` (não pergunta nada) · `-n/--dry-run` (não executa nada) · `--system` (escopo de sistema) · `--no-gpu` (não mexe em drivers) · `--studio` (instala o Studio junto) · `--no-open`.

---

## Problemas comuns e a solução real

| Sintoma | Causa | Solução |
|---|---|---|
| **Janela preta / fecha ao abrir** (NVIDIA) | Wayland + driver NVIDIA | No login, escolha a sessão **Xorg**. Ou force: `flatpak run --socket=x11 org.vinegarhq.Sober` |
| **Erro de Vulkan / "unsupported device"** | Driver Vulkan ausente ou velho | `./roblox-linux.sh install` instala; depois **reinicie a sessão**. AMD/Intel = Mesa; NVIDIA = driver proprietário da distro. |
| **FPS baixo (30–40)** | CPU emulando SSE4.2, ou GPU integrada | Confira no `doctor`: se mostrar SSE4.1 apenas, é limitação de hardware. Reduza a qualidade gráfica **dentro do jogo** (Roblox → Configurações → Gráficos). |
| **Sem áudio** | PipeWire/PulseAudio não exposto no sandbox | `flatpak run --socket=pulseaudio org.vinegarhq.Sober` (ou instale o override: `flatpak override --user --socket=pulseaudio org.vinegarhq.Sober`) |
| **Controle/gamepad não funciona** | Faltam permissões de dispositivo | `flatpak override --user --device=all org.vinegarhq.Sober` |
| **Parou de abrir depois de um update do Roblox** | O Roblox mudou o cliente; o Sober precisa acompanhar | `./roblox-linux.sh update`. O time do VinegarHQ costuma publicar correção em horas (houve janelas de ~18h–2 dias). |
| **Não aparece no menu de aplicativos** | Cache de atalhos | O `install` cria um `.desktop`; se ainda não aparecer, rode pelo terminal: `flatpak run org.vinegarhq.Sober` |
| **"Sober não encontrado no Flathub"** | Flathub não configurado | `flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo` |
| **Tela de login travada / não carrega** | Rede bloqueando os CDNs do Roblox | Teste em outra rede; verifique bloqueio de DNS/adblocker na rede. |

Diagnóstico geral: `./roblox-linux.sh doctor` · Log de cada execução: `/tmp/roblox-linux-<data>.log`.

---

## Roblox Studio (criar seus próprios jogos)

O **Studio** não tem Hyperion, então o caminho via Wine (**Vinegar**) funciona:

```bash
./roblox-linux.sh studio      # ou: flatpak install flathub org.vinegarhq.Vinegar
flatpak run org.vinegarhq.Vinegar
```

Na primeira execução o Vinegar prepara o Wine e baixa o Studio (~500 MB).

---

## FastFlags (avançado, com risco)

`./roblox-linux.sh fastflags` cria e abre o `ClientAppSettings.json` no lugar certo (`~/.var/app/org.vinegarhq.Sober/data/sober/exe/ClientSettings/`), **faz backup** do arquivo atual e valida o JSON antes de editar.

> ⚠️ FastFlags são um recurso não documentado da Roblox. Flags erradas (ou copiadas de fontes duvidosas) podem causar travamentos e **risco de banimento**. Este script **não** escreve nenhuma flag por você — ele só te dá o caminho certo e protege seu arquivo. Para reverter: deixe o arquivo com `{}`.

---

## Isso é seguro? (como auditar em 2 minutos)

1. `grep -n "curl\|wget" roblox-linux.sh` → **nenhum download de binário**; tudo vem do Flathub.
2. O Sober é distribuído em **Flatpak**, que roda em sandbox (permissões explícitas).
3. `flatpak info org.vinegarhq.Sober` mostra origem, versão e commit.
4. O APK do Roblox é baixado **dos servidores oficiais** pelo próprio Sober, com verificação de assinatura (os desenvolvedores documentam isso; binários modificados são rejeitados).
5. `./roblox-linux.sh --dry-run install` mostra cada comando **antes** de qualquer coisa acontecer.

---

## Testes

```bash
./test.sh              # 30 testes: sintaxe, dry-run, detecção, preflight, conteúdo
npm run test:linux     # idem, pelo npm
```

Os testes rodam em qualquer Linux, não instalam nada e cobrem inclusive os cenários de recusa (CPU sem SSE4.1, ARM, container) — garantindo que o script falha **antes** de mexer no sistema.

---

## FAQ rápido

**Funciona no Steam Deck?** Sim — é um dos melhores casos de uso (Modo Desktop para instalar, Modo Jogo para jogar).

**E no WSL2?** Não. O WSL2 não expõe Vulkan/GLES para o Linux, então o runtime gráfico não funciona. Use dual boot ou uma VM com GPU passthrough.

**Existe versão oficial?** Não. E não há sinal de que a Roblox vá lançar cliente Linux.

**Funciona para Roblox Studio também?** Sim, via Vinegar (`./roblox-linux.sh studio`).

**Quanto pesa?** Sober + runtime Flatpak ≈ 1,5 GB; o APK do Roblox, ~400 MB na primeira execução.

**Desinstalar?** `./roblox-linux.sh uninstall` (pergunta duas vezes: cliente e dados locais). Sua conta e seus jogos na Roblox não são afetados.
