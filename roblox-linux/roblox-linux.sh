#!/usr/bin/env bash
# =============================================================================
#  ROBLOX-LINUX — instalador e diagnóstico de Roblox para Linux
# =============================================================================
#  O que este software faz (e o que ele NÃO faz):
#
#   ✅ Automa o caminho que REALMENTE funciona em 2026: o Sober
#      (Flatpak org.vinegarhq.Sober, do time VinegarHQ), que executa o APK
#      OFICIAL do Roblox Android — baixado dos servidores oficiais da Google
#      Play e com verificação de assinatura. O anti-cheat Hyperion é
#      Windows-only e não se aplica a esse caminho.
#   ✅ Resolve os modos de falha que fazem 90% das pessoas desistirem:
#      flatpak ausente, Flathub não configurado, distro imutável (SteamOS,
#      Bazzite, Silverblue), driver Vulkan ausente, CPU sem SSE4.2,
#      kernel velho, espaço em disco, NVIDIA sem driver proprietário.
#   ✅ Instala o Roblox Studio via Vinegar (opcional, com --studio).
#   ✅ Diagnostica e explica exatamente o que corrigir.
#
#   ❌ NÃO cria, modifica, decompila ou falsifica cliente do Roblox.
#   ❌ NÃO contorna anti-cheat, NÃO altera Binários do jogo.
#   ❌ NÃO conversa com os servidores da Roblox por conta própria.
#      Quem faz isso é o cliente OFICIAL deles, rodando dentro do runtime.
#
#  Aviso honesto: o Roblox não publica cliente para Linux nem endossa o Sober.
#  Usar é decisão sua — não há ondas de banimento reportadas até 05/2026, e o
#  servidor vê você como um jogador Android comum.
#
#  Uso:  ./roblox-linux.sh [comando] [opções]
#        ./roblox-linux.sh               → menu interativo
#        ./roblox-linux.sh doctor        → diagnóstico completo
#        ./roblox-linux.sh install       → instala tudo
#        ./roblox-linux.sh --help        → lista completa
# =============================================================================

set -Eeuo pipefail

TOOL_VERSION="1.0.0"
SOBER_ID="org.vinegarhq.Sober"
VINEGAR_ID="org.vinegarhq.Vinegar"
FLATHUB_URL="https://dl.flathub.org/repo/flathub.flatpakrepo"
SOBER_REF_URL="https://sober.vinegarhq.org/sober.flatpakref"

# ── Opções (podem ser ligadas por linha de comando) ──────────────────────────
DRY_RUN=0        # mostra o plano sem executar nada
ASSUME_YES=0     # não pergunta nada
USER_SCOPE=1     # instala no escopo do usuário (sem sudo, funciona em imutáveis)
INSTALL_STUDIO=0 # instala Vinegar (Studio) junto
INSTALL_GPU=1    # tenta instalar drivers Vulkan faltantes
OPEN_AFTER=1     # abre o Roblox ao terminar
LOG_FILE="${TMPDIR:-/tmp}/roblox-linux-$(date +%Y%m%d-%H%M%S).log"

# ── Cores (só quando o terminal suporta) ─────────────────────────────────────
if [ -t 1 ] && [ "${NO_COLOR:-}" = "" ]; then
  C_R='\033[0m'; C_B='\033[1m'; C_DIM='\033[2m'
  C_GRN='\033[38;5;46m'; C_YEL='\033[38;5;220m'; C_RED='\033[38;5;203m'
  C_CYA='\033[38;5;45m'; C_MAG='\033[38;5;177m'
else
  C_R=''; C_B=''; C_DIM=''; C_GRN=''; C_YEL=''; C_RED=''; C_CYA=''; C_MAG=''
fi

# ── Saída padronizada ────────────────────────────────────────────────────────
log()   { printf '%s\n' "$*" >>"$LOG_FILE" 2>/dev/null || true; }
say()   { printf '  %s\n' "$*"; log "$*"; }
ok()    { printf '  %b✔%b  %s\n' "$C_GRN" "$C_R" "$*"; log "OK: $*"; }
warn()  { printf '  %b▲%b  %s\n' "$C_YEL" "$C_R" "$*"; log "AVISO: $*"; }
err()   { printf '  %b✖%b  %s\n' "$C_RED" "$C_R" "$*" >&2; log "ERRO: $*"; }
info()  { printf '  %b·%b  %s\n' "$C_CYA" "$C_R" "$*"; log "INFO: $*"; }
fix()   { printf '     %b↳%b %b%s%b\n' "$C_MAG" "$C_R" "$C_YEL" "$*" "$C_R"; log "CORRIGIR: $*"; }
title() { printf '\n%b%s%b\n' "$C_B" "$*" "$C_R"; log "== $* =="; }
rule()  { printf '%b%s%b\n' "$C_DIM" "  ─────────────────────────────────────────────────────────────" "$C_R"; }

# Executa (ou apenas mostra, em --dry-run). Toda ação destrutiva passa por aqui.
run() {
  if [ "$DRY_RUN" = "1" ]; then
    printf '     %b[dry-run]%b %s\n' "$C_DIM" "$C_R" "$*"
    log "DRY-RUN: $*"
    return 0
  fi
  log "EXEC: $*"
  "$@"
}

# Executa com sudo quando necessário; em dry-run, mostra.
run_root() {
  if [ "$(id -u)" = "0" ]; then run "$@"; return; fi
  if command -v sudo >/dev/null 2>&1; then run sudo "$@"; return; fi
  err "preciso de privilégios de root e o 'sudo' não está disponível."
  err "Rode como root ou instale o sudo (comando: $*)"
  return 1
}

ask() { # ask "pergunta" → 0 = sim
  if [ "$ASSUME_YES" = "1" ]; then return 0; fi
  if [ ! -t 0 ]; then return 0; fi
  local reply
  printf '     %b?%b %s [S/n] ' "$C_CYA" "$C_R" "$1"
  read -r reply || true
  case "${reply:-s}" in s|S|y|Y|"") return 0 ;; *) return 1 ;; esac
}

have() { command -v "$1" >/dev/null 2>&1; }

on_error() {
  local code=$?
  printf '\n' >&2
  err "falhou no passo anterior (código $code)."
  err "log completo: $LOG_FILE"
  err "Rode: $0 doctor   — para ver o que está faltando."
  exit "$code"
}
trap on_error ERR

# =============================================================================
#  DETECÇÃO DO SISTEMA
# =============================================================================

declare -A SYS=()

detect_system() {
  # ── Distro e família de pacotes ────────────────────────────────────────────
  SYS[distro]="desconhecida"; SYS[distro_name]="Linux"; SYS[distro_like]=""; SYS[pkg]="desconhecido"
  if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    SYS[distro]="${ID:-desconhecida}"
    SYS[distro_name]="${PRETTY_NAME:-${NAME:-Linux}}"
    SYS[distro_like]="${ID_LIKE:-}"
    SYS[version]="${VERSION_ID:-}"
  fi
  if [ -r /etc/lsb-release ] && [ "${SYS[distro]}" = "desconhecida" ]; then
    # shellcheck disable=SC1091
    . /etc/lsb-release; SYS[distro]="$(printf '%s' "${DISTRIB_ID:-}" | tr 'A-Z' 'a-z')"
  fi

  case "${SYS[distro]} ${SYS[distro_like]}" in
    *debian*|*ubuntu*|*mint*|*pop*|*zorin*|*kali*|*elementary*|*raspbian*) SYS[pkg]="apt" ;;
    *fedora*|*rhel*|*centos*|*rocky*|*almalinux*|*nobara*|*bazzite*|*silverblue*) SYS[pkg]="dnf" ;;
    *arch*|*manjaro*|*endeavour*|*garuda*|*cachyos*) SYS[pkg]="pacman" ;;
    *opensuse*|*suse*|*sles*) SYS[pkg]="zypper" ;;
    *alpine*) SYS[pkg]="apk" ;;
    *void*) SYS[pkg]="xbps" ;;
    *steamos*) SYS[pkg]="pacman" ;;
  esac

  # ── Distro imutável? (SteamOS, Bazzite, Silverblue/Kinoite, NixOS) ─────────
  SYS[immutable]="0"
  if [ -e /run/ostree-booted ]; then SYS[immutable]="1"; fi
  case "${SYS[distro]}" in
    steamos|bazzite|silverblue|kinoite|fedora-silverblue|endless|nixos) SYS[immutable]="1" ;;
  esac

  # ── Hardware ──────────────────────────────────────────────────────────────
  SYS[arch]="$(uname -m)"
  SYS[kernel]="$(uname -r)"
  SYS[kernel_ok]="0"
  # compara versão: >= 5.11
  local kmaj kmin
  kmaj="$(printf '%s' "${SYS[kernel]}" | cut -d. -f1)"
  kmin="$(printf '%s' "${SYS[kernel]}" | cut -d. -f2 | tr -cd '0-9')"
  if [ "${kmaj:-0}" -gt 5 ] 2>/dev/null || { [ "${kmaj:-0}" -eq 5 ] && [ "${kmin:-0}" -ge 11 ]; } 2>/dev/null; then
    SYS[kernel_ok]="1"
  fi

  SYS[sse41]="0"; SYS[sse42]="0"
  if [ -r /proc/cpuinfo ]; then
    if grep -qw sse4_1 /proc/cpuinfo; then SYS[sse41]="1"; fi
    if grep -qw sse4_2 /proc/cpuinfo; then SYS[sse42]="1"; fi
  fi
  SYS[cores]="$(nproc 2>/dev/null || echo '?')"

  # ── Memória RAM total (MB) ────────────────────────────────────────────────
  SYS[ram_mb]="0"
  if [ -r /proc/meminfo ]; then
    SYS[ram_mb]="$(awk '/MemTotal/ {printf "%d", $2/1024}' /proc/meminfo)"
  fi

  # ── GPU ───────────────────────────────────────────────────────────────────
  SYS[gpu_vendor]="desconhecida"; SYS[gpu_name]=""
  if have lspci; then
    local gpu
    gpu="$(lspci 2>/dev/null | grep -iE 'vga|3d|display' | head -1 || true)"
    SYS[gpu_name]="$(printf '%s' "$gpu" | sed 's/.*: //')"
    case "$gpu" in
      *NVIDIA*|*nVidia*|*GeForce*|*Quadro*) SYS[gpu_vendor]="nvidia" ;;
      *AMD*|*Radeon*|*ATI*)               SYS[gpu_vendor]="amd" ;;
      *Intel*)                            SYS[gpu_vendor]="intel" ;;
    esac
  fi
  # Fallback por módulo de kernel carregado
  if [ "${SYS[gpu_vendor]}" = "desconhecida" ] && [ -r /proc/modules ]; then
    if grep -q '^nvidia ' /proc/modules; then SYS[gpu_vendor]="nvidia"; fi
    if grep -qE '^(amdgpu|radeon) ' /proc/modules; then SYS[gpu_vendor]="amd"; fi
    if grep -q '^i915 ' /proc/modules; then SYS[gpu_vendor]="intel"; fi
  fi

  SYS[vulkan_icd]="0"
  if [ -d /usr/share/vulkan/icd.d ] && ls /usr/share/vulkan/icd.d/*.json >/dev/null 2>&1; then SYS[vulkan_icd]="1"; fi
  if [ -d /etc/vulkan/icd.d ] && ls /etc/vulkan/icd.d/*.json >/dev/null 2>&1; then SYS[vulkan_icd]="1"; fi
  SYS[gl_es]="0"
  if [ -e /usr/lib/x86_64-linux-gnu/libGLESv2.so.2 ] || [ -e /usr/lib64/libGLESv2.so.2 ] || [ -e /usr/lib/libGLESv2.so.2 ]; then SYS[gl_es]="1"; fi
  SYS[nvidia_driver]=""
  if have nvidia-smi; then SYS[nvidia_driver]="$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1 || true)"; fi

  # ── Sessão gráfica e container ────────────────────────────────────────────
  SYS[session]="${XDG_SESSION_TYPE:-desconhecida}"
  SYS[desktop]="${XDG_CURRENT_DESKTOP:-?}"
  SYS[wayland]="0"
  if [ "${SYS[session]}" = "wayland" ]; then SYS[wayland]="1"; fi
  SYS[container]="0"
  if [ -e /.dockerenv ] || grep -qaE 'docker|containerd|podman|lxc' /proc/1/cgroup 2>/dev/null; then SYS[container]="1"; fi

  # ── Flatpak / Flathub / Sober ─────────────────────────────────────────────
  SYS[flatpak]="0"; SYS[flatpak_ver]=""; SYS[flathub]="0"
  SYS[sober]="0"; SYS[sober_ver]=""; SYS[sober_scope]=""
  SYS[vinegar]="0"
  if have flatpak; then
    SYS[flatpak]="1"
    SYS[flatpak_ver]="$(flatpak --version 2>/dev/null | awk '{print $2}' || true)"
    if flatpak remotes --columns=name 2>/dev/null | grep -qx flathub; then SYS[flathub]="1"; fi
    if flatpak info "$SOBER_ID" >/dev/null 2>&1; then
      SYS[sober]="1"
      SYS[sober_ver]="$(flatpak info "$SOBER_ID" 2>/dev/null | awk -F': *' '/Version/ {print $2; exit}' || true)"
      SYS[sober_scope]="system"
    elif flatpak info --user "$SOBER_ID" >/dev/null 2>&1; then
      SYS[sober]="1"
      SYS[sober_ver]="$(flatpak info --user "$SOBER_ID" 2>/dev/null | awk -F': *' '/Version/ {print $2; exit}' || true)"
      SYS[sober_scope]="user"
    fi
    if flatpak info "$VINEGAR_ID" >/dev/null 2>&1 || flatpak info --user "$VINEGAR_ID" >/dev/null 2>&1; then SYS[vinegar]="1"; fi
  fi

  # ── Espaço em disco (onde o flatpak vive) ────────────────────────────────
  local target="${HOME}/.var" ; [ -d "$target" ] || target="$HOME"
  SYS[disk_gb]="$(df -BG --output=avail "$target" 2>/dev/null | tail -1 | tr -cd '0-9' || echo '0')"
  if [ -z "${SYS[disk_gb]:-}" ]; then SYS[disk_gb]="0"; fi
}

# =============================================================================
#  DIAGNÓSTICO (doctor)
# =============================================================================
DOCTOR_CODES=0

check_ok()   { ok "$1"; }
check_warn() { warn "$1"; DOCTOR_CODES=$((DOCTOR_CODES + 1)); if [ -n "${2:-}" ]; then fix "$2"; fi; }
check_fail() { err "$1"; DOCTOR_CODES=$((DOCTOR_CODES + 2)); if [ -n "${2:-}" ]; then fix "$2"; fi; }

cmd_doctor() {
  detect_system
  printf '\n%b  ROBLOX-LINUX %s — diagnóstico do sistema%b\n' "$C_B" "$TOOL_VERSION" "$C_R"
  rule
  printf '  %b%s%b\n' "$C_CYA" "${SYS[distro_name]} · kernel ${SYS[kernel]} · ${SYS[arch]} · ${SYS[cores]} núcleos · ${SYS[ram_mb]} MB RAM" "$C_R"
  rule

  title "1. Requisitos de hardware e kernel"
  if [ "${SYS[arch]}" = "x86_64" ]; then check_ok "Arquitetura x86-64 (o APK do Roblox exige)"; else
    check_fail "Arquitetura ${SYS[arch]} — o Sober precisa de x86-64" "Em ARM (Raspberry Pi, Snapdragon X) não há caminho para o Roblox no Linux hoje."
  fi
  if [ "${SYS[kernel_ok]}" = "1" ]; then check_ok "Kernel ${SYS[kernel]} (≥ 5.11)"; else
    check_warn "Kernel ${SYS[kernel]} — o Sober pede ≥ 5.11" "Atualize o kernel (Ubuntu: sudo apt install linux-generic; Fedora: sudo dnf upgrade kernel)"
  fi
  if [ "${SYS[sse42]}" = "1" ]; then check_ok "CPU com SSE4.2 (execução nativa, sem emulação)"
  elif [ "${SYS[sse41]}" = "1" ]; then
    check_warn "CPU só tem SSE4.1 — o Sober emula SSE4.2 (roda, mas com menos FPS)" "Se o desempenho ficar ruim, esse é o motivo — não há solução por software."
  else
    check_fail "CPU sem SSE4.1 — o APK x86_64 do Roblox NÃO roda" "CPUs anteriores a ~2011 (Intel Core 1ª geração / AMD K10) não são suportadas."
  fi
  if [ "${SYS[ram_mb]:-0}" -ge 4000 ] 2>/dev/null; then check_ok "Memória: ${SYS[ram_mb]} MB (≥ 4 GB)"; else
    check_warn "Memória: ${SYS[ram_mb]} MB — 4 GB é o mínimo confortável"
  fi
  if [ "${SYS[disk_gb]:-0}" -ge 4 ] 2>/dev/null; then check_ok "Espaço livre: ${SYS[disk_gb]} GB (o Sober + runtime usam ~1,5 GB)"; else
    check_warn "Espaço livre: ${SYS[disk_gb]} GB — libere ao menos 4 GB"
  fi

  title "2. Placa de vídeo e drivers"
  if [ -n "${SYS[gpu_name]}" ]; then check_ok "GPU: ${SYS[gpu_name]}"; else check_warn "GPU não identificada (sem lspci?) — siga se o jogo abrir"; fi
  case "${SYS[gpu_vendor]}" in
    nvidia)
      if [ -n "${SYS[nvidia_driver]}" ]; then
        check_ok "Driver NVIDIA proprietário ativo (${SYS[nvidia_driver]})"
      else
        check_fail "GPU NVIDIA sem driver proprietário (binário nouveau não basta)" \
          "$(pkg_cmd_for 'nvidia-driver' 'akmod-nvidia' 'nvidia' 'nvidia-open')"
      fi ;;
    amd)
      if [ "${SYS[vulkan_icd]}" = "1" ]; then check_ok "Mesa/Vulkan presente para AMD (radeon vulkan)"; else
        check_fail "Driver Vulkan da AMD ausente" "$(pkg_cmd_for 'mesa-vulkan-drivers' 'mesa-vulkan-drivers' 'vulkan-radeon' 'Mesa-libvulkan-devel')"
      fi ;;
    intel)
      if [ "${SYS[vulkan_icd]}" = "1" ]; then check_ok "Mesa/Vulkan presente para Intel"; else
        check_fail "Driver Vulkan da Intel ausente" "$(pkg_cmd_for 'mesa-vulkan-drivers' 'mesa-vulkan-drivers' 'vulkan-intel' 'Mesa-libvulkan-devel')"
      fi ;;
    *)
      if [ "${SYS[vulkan_icd]}" = "1" ]; then check_ok "ICDs do Vulkan instalados"; else
        check_warn "Nenhum driver Vulkan detectado (o Sober aceita OpenGL ES 3.0, mas Vulkan é mais rápido)" \
          "$(pkg_cmd_for 'mesa-vulkan-drivers' 'mesa-vulkan-drivers' 'vulkan-radeon vulkan-intel' 'Mesa-libvulkan-devel')"
      fi ;;
  esac
  if [ "${SYS[gl_es]}" = "1" ]; then check_ok "OpenGL ES 3.x disponível (requisito mínimo do Sober)"; else
    check_warn "libGLESv2 não encontrada — pode faltar o runtime 32/64 bits do Mesa" "$(pkg_cmd_for 'libgles2' 'mesa-libGLES' 'libglvnd' 'libGLESv2')"
  fi
  if [ "${SYS[gpu_vendor]}" = "nvidia" ] && [ "${SYS[wayland]}" = "1" ]; then
    info "Sessão Wayland + NVIDIA: se a janela abrir preta, troque a sessão para Xorg no login."
  fi

  title "3. Flatpak (o único canal de distribuição do Sober)"
  if [ "${SYS[flatpak]}" = "1" ]; then
    check_ok "Flatpak instalado (${SYS[flatpak_ver]:-versão ?})"
    if [ "${SYS[flathub]}" = "1" ]; then check_ok "Repositório Flathub configurado"; else
      check_fail "Flathub não configurado — sem ele o Sober não é encontrado" \
        "flatpak remote-add --if-not-exists --user flathub $FLATHUB_URL"
    fi
  else
    if [ "${SYS[immutable]}" = "1" ]; then
      check_fail "Flatpak ausente em distro IMUTÁVEL (${SYS[distro]})" \
        "use: flatpak já vem incluso normalmente; se não vier, instale via camada: sudo rpm-ostree install flatpak (e reinicie)"
    else
      check_fail "Flatpak ausente" "$(pkg_cmd_for 'flatpak' 'flatpak' 'flatpak' 'flatpak')"
    fi
  fi

  title "4. Estado do Roblox (Sober / Studio)"
  if [ "${SYS[sober]}" = "1" ]; then
    check_ok "Sober instalado (${SYS[sober_ver]:-versão ?}, escopo ${SYS[sober_scope]})"
    info "Abra com: flatpak run $SOBER_ID"
  else
    check_warn "Sober ainda não instalado" "Rode: $0 install"
  fi
  if [ "${SYS[vinegar]}" = "1" ]; then check_ok "Vinegar presente (Roblox Studio)"; else
    info "Studio (criar jogos) não instalado — opcional: $0 studio"
  fi

  title "5. Ambiente"
  if [ "${SYS[container]}" = "1" ]; then
    warn "Parece um container — dentro de container o Sober normalmente NÃO funciona"
    fix "Rode no sistema hospedeiro (não em Docker/WSL). No WSL2 o Vulkan não é exposto: também não serve."
  else
    check_ok "Sistema real (não container)"
  fi
  info "Sessão: ${SYS[session]} · desktop: ${SYS[desktop]}"

  rule
  if [ "$DOCTOR_CODES" -eq 0 ]; then
    printf '  %b✅ Tudo pronto para o Roblox.%b  Abra com: %bflatpak run %s%b\n\n' "$C_GRN" "$C_R" "$C_B" "$SOBER_ID" "$C_R"
    return 0
  fi
  printf '  %b⚠ %d item(ns) merecem atenção acima (marcados com ↳).%b\n' "$C_YEL" "$DOCTOR_CODES" "$C_R"
  if [ "${SYS[sober]}" != "1" ]; then
    printf '  Próximo passo: %b%s install%b\n\n' "$C_B" "$0" "$C_R"
  fi
  return 0
}

# Sugere o comando de instalação certo para a distro detectada.
# Uso: pkg_cmd_for <apt> <dnf> <pacman> <zypper>
pkg_cmd_for() {
  local a="$1" b="$2" c="$3" d="$4"
  case "${SYS[pkg]:-}" in
    apt)    printf 'sudo apt install %s' "$a" ;;
    dnf)    printf 'sudo dnf install %s' "$b" ;;
    pacman) printf 'sudo pacman -S %s' "$c" ;;
    zypper) printf 'sudo zypper install %s' "$d" ;;
    apk)    printf 'sudo apk add %s' "$a" ;;
    xbps)   printf 'sudo xbps-install %s' "$a" ;;
    *)      printf 'instale o pacote equivalente a "%s" no seu gerenciador' "$a" ;;
  esac
}

# =============================================================================
#  INSTALAÇÃO
# =============================================================================

ensure_flatpak() {
  title "1/5 · Flatpak"
  if [ "${SYS[flatpak]}" = "1" ]; then ok "Flatpak já instalado (${SYS[flatpak_ver:-?})"; return 0; fi
  if [ "${SYS[immutable]}" = "1" ]; then
    err "distro imutável sem flatpak — instale pela camada do sistema e reinicie:"
    fix "sudo rpm-ostree install flatpak   (Fedora Silverblue/Bazzite/Kinoite)"
    fix "No SteamOS, ative o modo desenvolvedor e use: sudo steamos-readonly disable && sudo pacman -S flatpak"
    return 1
  fi
  info "Instalando o Flatpak via ${SYS[pkg]}…"
  case "${SYS[pkg]}" in
    apt)    run_root apt-get update -qq && run_root apt-get install -y flatpak ;;
    dnf)    run_root dnf install -y flatpak ;;
    pacman) run_root pacman -S --needed --noconfirm flatpak ;;
    zypper) run_root zypper --non-interactive install flatpak ;;
    apk)    run_root apk add flatpak ;;
    xbps)   run_root xbps-install -y flatpak ;;
    *)      err "gerenciador de pacotes não identificado (${SYS[pkg]})."
            fix "Siga https://flatpak.org/setup/ e rode este instalador de novo."
            return 1 ;;
  esac
  ok "Flatpak instalado"
}

ensure_flathub() {
  title "2/5 · Repositório Flathub"
  if [ "$DRY_RUN" = "0" ] && flatpak remotes --columns=name 2>/dev/null | grep -qx flathub; then
    ok "Flathub já configurado"; return 0
  fi
  run flatpak remote-add --if-not-exists --user flathub "$FLATHUB_URL"
  # Em algumas distros o Flathub já vem no escopo de sistema; adicionar no
  # escopo do usuário é inofensivo e garante o funcionamento sem sudo.
  ok "Flathub configurado"
}

ensure_gpu_drivers() {
  title "3/5 · Drivers de vídeo (Vulkan / OpenGL ES)"
  if [ "$INSTALL_GPU" != "1" ]; then info "pulado (--no-gpu)"; return 0; fi
  if [ "${SYS[vulkan_icd]}" = "1" ] && [ "${SYS[gl_es]}" = "1" ]; then
    ok "Vulkan e OpenGL ES já disponíveis"; return 0
  fi

  local pkgs_apt="" pkgs_dnf="" pkgs_pacman="" pkgs_zypper=""
  case "${SYS[gpu_vendor]}" in
    amd)   pkgs_apt="mesa-vulkan-drivers mesa-vdpau-drivers"; pkgs_dnf="mesa-vulkan-drivers";   pkgs_pacman="vulkan-radeon mesa"; pkgs_zypper="Mesa-libvulkan-devel" ;;
    intel) pkgs_apt="mesa-vulkan-drivers";                    pkgs_dnf="mesa-vulkan-drivers";   pkgs_pacman="vulkan-intel mesa"; pkgs_zypper="Mesa-libvulkan-devel" ;;
    nvidia)
      pkgs_apt="nvidia-driver libnvidia-gl"; pkgs_dnf="akmod-nvidia xorg-x11-drv-nvidia-cuda"; pkgs_pacman="nvidia nvidia-utils"; pkgs_zypper="nvidia-video-G06" ;;
    *)     pkgs_apt="mesa-vulkan-drivers"; pkgs_dnf="mesa-vulkan-drivers"; pkgs_pacman="vulkan-radeon vulkan-intel mesa"; pkgs_zypper="Mesa-libvulkan-devel" ;;
  esac
  # libGLES sempre ajuda (Mesa/glvnd)
  case "${SYS[pkg]}" in
    apt)    pkgs_apt="$pkgs_apt libgl1 libgles2" ;;
    dnf)    pkgs_dnf="$pkgs_dnf mesa-libGLES mesa-libglvnd" ;;
    pacman) pkgs_pacman="$pkgs_pacman libglvnd" ;;
  esac

  if [ "${SYS[gpu_vendor]}" = "nvidia" ]; then
    warn "NVIDIA detectada: o driver proprietário deve vir do repositório da SUA distro (não do site da NVIDIA)."
    info "Comando sugerido: $(pkg_cmd_for "$pkgs_apt" "$pkgs_dnf" "$pkgs_pacman" "$pkgs_zypper")"
    info "Depois de instalar o driver, REINICIE a máquina antes de abrir o Roblox."
  else
    info "Instalando: $(pkg_cmd_for "$pkgs_apt" "$pkgs_dnf" "$pkgs_pacman" "$pkgs_zypper")"
  fi

  if ! ask "Instalar os drivers de vídeo agora? (precisa de senha de administrador)"; then
    warn "pulado — o Sober pode abrir sem Vulkan, mas com menos desempenho"
    return 0
  fi

  case "${SYS[pkg]}" in
    apt)    run_root apt-get update -qq; run_root apt-get install -y $pkgs_apt ;;
    dnf)    run_root dnf install -y $pkgs_dnf ;;
    pacman) run_root pacman -S --needed --noconfirm $pkgs_pacman ;;
    zypper) run_root zypper --non-interactive install $pkgs_zypper ;;
    *)      warn "instale manualmente pelo seu gerenciador"; return 0 ;;
  esac
  ok "Drivers instalados (se o Vulkan ainda não aparecer, reinicie a sessão gráfica)"
}

install_sober() {
  title "4/5 · Roblox (Sober)"
  if [ "${SYS[sober]}" = "1" ]; then
    ok "Sober já instalado (${SYS[sober_ver:-?}, escopo ${SYS[sober_scope]})"
    info "Atualizando para a última versão (o Roblox muda com frequência e o Sober acompanha)…"
    run flatpak update -y "$SOBER_ID" || warn "não foi possível atualizar agora (siga em frente, o instalado funciona)"
    return 0
  fi

  local scope_flag="--user"
  if [ "$USER_SCOPE" = "0" ]; then scope_flag=""; fi

  info "Baixando o Sober do Flathub (~600 MB, inclui o runtime do Flatpak)…"
  info "Isso é o cliente do Roblox para Android, baixado DOS SERVIDORES OFICIAIS da Roblox."
  if [ "$DRY_RUN" = "0" ]; then
    if ! flatpak install -y $scope_flag flathub "$SOBER_ID"; then
      err "a instalação pelo Flathub falhou."
      info "Tentando o canal direto do projeto (mesmo pacote, remote próprio)…"
      run flatpak install -y $scope_flag "$SOBER_REF_URL" || {
        err "os dois canais falharam. Verifique sua conexão e rode: $0 doctor"; return 1; }
    fi
  else
    run flatpak install -y $scope_flag flathub "$SOBER_ID"
  fi
  ok "Sober instalado"
}

ensure_launcher() {
  title "5/5 · Atalho no menu de aplicativos"
  local dir="$HOME/.local/share/applications"
  local file="$dir/roblox-linux.desktop"
  if [ -f "$file" ]; then ok "Atalho já existe"; return 0; fi
  if [ "$DRY_RUN" = "1" ]; then run mkdir -p "$dir"; info "[dry-run] criaria $file"; return 0; fi
  mkdir -p "$dir"
  cat >"$file" <<EOF
[Desktop Entry]
Type=Application
Name=Roblox
Name[pt_BR]=Roblox (Sober)
Comment=Roblox para Linux via Sober
Exec=flatpak run $SOBER_ID %u
Terminal=false
Categories=Game;
Keywords=roblox;sober;game;
StartupNotify=true
EOF
  update-desktop-database "$dir" >/dev/null 2>&1 || true
  ok "Atalho criado (procure por “Roblox” no menu)"
}

# Impedimentos reais para rodar o Roblox no Linux. Isolado em função pura
# (recebe os dados por parâmetro) para poder ser testado sem inspecionar o
# hardware de verdade. Devolve 1 quando não há caminho nenhum.
# Uso: preflight <arch> <sse41> <sse42> <container>
preflight() {
  local arch="$1" sse41="$2" sse42="$3" container="$4"
  if [ "$arch" != "x86_64" ]; then
    err "arquitetura $arch não é suportada (o APK do Roblox é x86_64)."
    fix "Em ARM (Raspberry Pi, Snapdragon) não existe caminho para o Roblox no Linux hoje."
    return 1
  fi
  if [ "$sse41" != "1" ]; then
    err "esta CPU não tem SSE4.1 — o cliente do Roblox não consegue rodar."
    fix "Não há contorno por software: só funciona em outra máquina."
    return 1
  fi
  if [ "$container" = "1" ]; then
    warn "detectado ambiente de container: o Sober normalmente não funciona aqui."
    fix "Rode este instalador no sistema hospedeiro (no WSL2 o Vulkan não é exposto)."
    ask "Continuar mesmo assim?" || return 1
  fi
  if [ "$sse42" != "1" ]; then warn "CPU sem SSE4.2: o Sober vai emular (menos FPS)."; fi
  return 0
}

cmd_install() {
  detect_system
  printf '\n%b  ROBLOX-LINUX %s — instalando o Roblox%b\n' "$C_B" "$TOOL_VERSION" "$C_R"
  rule
  printf '  Sistema: %s (%s) · %s · kernel %s\n' "${SYS[distro_name]}" "${SYS[pkg]}" "${SYS[arch]}" "${SYS[kernel]}"
  if [ "$DRY_RUN" = "1" ]; then printf '  %bMODO SIMULAÇÃO (--dry-run): nada será alterado.%b\n' "$C_YEL" "$C_R"; fi
  rule

  # Bloqueios reais (função pura → testável sem tocar no hardware)
  preflight "${SYS[arch]}" "${SYS[sse41]}" "${SYS[sse42]}" "${SYS[container]}" || return 1

  ensure_flatpak
  ensure_flathub
  ensure_gpu_drivers
  install_sober
  ensure_launcher

  if [ "$INSTALL_STUDIO" = "1" ]; then cmd_studio; fi

  title "Pronto"
  ok "Roblox instalado. Abra pelo menu de aplicativos ou com: flatpak run $SOBER_ID"
  info "No PRIMEIRO uso: escolha “Automatic”, faça login e deixe baixar (~400 MB)."
  info "Depois disso, é só jogar — e usar $0 update para atualizar quando o Roblox mudar."
  if [ "${SYS[sse42]}" != "1" ]; then warn "Lembre-se: sua CPU emula SSE4.2, então espere FPS mais baixo."; fi
  if [ "$OPEN_AFTER" = "1" ] && [ "$DRY_RUN" = "0" ] && ask "Abrir o Roblox agora?"; then cmd_launch; fi
}

# =============================================================================
#  COMANDOS DE MANUTENÇÃO
# =============================================================================

cmd_launch() {
  detect_system
  if [ "${SYS[sober]}" != "1" ]; then err "Sober não instalado. Rode: $0 install"; return 1; fi
  info "Iniciando o Roblox (Sober)…"
  exec flatpak run "$SOBER_ID" "$@"
}

cmd_update() {
  detect_system
  if [ "${SYS[flatpak]}" != "1" ]; then err "Flatpak ausente."; return 1; fi
  title "Atualizando Sober + runtimes"
  run flatpak update -y
  ok "Tudo atualizado"
  info "Se o Roblox parar de abrir após um update do jogo, espere algumas horas:"
  info "o time do VinegarHQ publica a correção junto com a mudança da Roblox."
}

cmd_studio() {
  detect_system
  title "Roblox Studio (Vinegar)"
  info "O Studio NÃO tem anti-cheat Hyperion, por isso o caminho via Wine (Vinegar) funciona."
  info "Sem ele você só joga; com ele você cria e publica seus próprios jogos."
  if ! ask "Instalar o Vinegar (Roblox Studio) agora?"; then return 0; fi
  run flatpak install -y flathub "$VINEGAR_ID"
  ok "Vinegar instalado — abra com: flatpak run $VINEGAR_ID"
  info "Na primeira execução ele configura o Wine e baixa o Studio."
}

FASTFLAGS_DIR="$HOME/.var/app/${SOBER_ID}/data/sober/exe/ClientSettings"
FASTFLAGS_FILE="$FASTFLAGS_DIR/ClientAppSettings.json"

cmd_fastflags() {
  detect_system
  title "FastFlags (ajustes avançados do cliente)"
  warn "FastFlag é um recurso NÃO documentado da Roblox. Flags erradas ou de fonte"
  warn "duvidosa podem causar instabilidade e risco de banimento. Use com consciência."
  mkdir -p "$FASTFLAGS_DIR"
  if [ -f "$FASTFLAGS_FILE" ]; then
    if have python3 && ! python3 -c "import json,sys;json.load(open('$FASTFLAGS_FILE'))" 2>/dev/null; then
      err "o arquivo atual tem JSON inválido — nada será alterado."
      fix "Corrija ou remova: $FASTFLAGS_FILE"
      return 1
    fi
    cp -f "$FASTFLAGS_FILE" "$FASTFLAGS_FILE.bak.$(date +%s)"
    ok "Backup criado antes de editar"
  else
    printf '{\n}\n' >"$FASTFLAGS_FILE"
    ok "Arquivo criado vazio"
  fi
  info "Arquivo: $FASTFLAGS_FILE"
  info "Formato: pares \"NomeDaFlag\": true/false/valor"
  if [ -n "${EDITOR:-}" ] && have "$EDITOR"; then
    ask "Abrir no editor ${EDITOR}?" && "$EDITOR" "$FASTFLAGS_FILE"
  else
    fix "Abra manualmente: nano \"$FASTFLAGS_FILE\""
  fi
  info "Para reverter tudo: apague as linhas e deixe apenas {} — o cliente volta ao padrão."
}

cmd_uninstall() {
  detect_system
  title "Remover o Roblox (Sober)"
  warn "Isso apaga o cliente E os dados locais (login, cache de jogos)."
  warn "Seus jogos e sua conta na Roblox NÃO são afetados."
  ask "Confirmar remoção?" || { info "cancelado"; return 0; }
  run flatpak uninstall -y "$SOBER_ID" || true
  if ask "Apagar também os dados locais (~/.var/app/$SOBER_ID)?"; then
    run rm -rf "$HOME/.var/app/$SOBER_ID"
    ok "Dados removidos"
  fi
  run rm -f "$HOME/.local/share/applications/roblox-linux.desktop"
  ok "Sober removido"
  if [ "${SYS[vinegar]}" = "1" ]; then info "O Vinegar (Studio) continua instalado. Remova com: flatpak uninstall $VINEGAR_ID"; fi
}

cmd_help() {
  cat <<EOF

  ${C_B}ROBLOX-LINUX ${TOOL_VERSION}${C_R} — Roblox no Linux pelo caminho que funciona hoje

  ${C_B}USO${C_R}
    ./roblox-linux.sh                  Menu interativo
    ./roblox-linux.sh install          Instala tudo (Flatpak + Flathub + drivers + Sober)
    ./roblox-linux.sh doctor           Diagnóstico do sistema, com a correção de cada item
    ./roblox-linux.sh launch           Abre o Roblox
    ./roblox-linux.sh update           Atualiza o Sober (o Roblox quebra com frequência)
    ./roblox-linux.sh studio           Instala o Roblox Studio via Vinegar (criar jogos)
    ./roblox-linux.sh fastflags        Abre os ajustes avançados do cliente (risco: leia o aviso)
    ./roblox-linux.sh uninstall        Remove tudo

  ${C_B}OPÇÕES${C_R}
    -y, --yes          Não pergunta nada (útil para automação)
    -n, --dry-run      Mostra o plano sem executar NADA (seguro: rode isso primeiro)
    --system           Instala o Flatpak no escopo do sistema (pede sudo)
    --no-gpu           Não mexe em drivers de vídeo
    --studio           Instala o Studio junto
    --no-open          Não oferece abrir o jogo no final
    -h, --help         Esta ajuda

  ${C_B}O QUE ELE FAZ POR BAIXO${C_R}
    Instala o Sober (Flatpak org.vinegarhq.Sober, do Flathub): um runtime que
    executa o APK OFICIAL do Roblox para Android — baixado dos servidores
    oficiais. O anti-cheat Hyperion é Windows-only e não se aplica a esse
    caminho. Não criamos, alteramos nem falsificamos cliente nenhum, e não
    contornamos anti-cheat.

  ${C_B}AVISO${C_R}
    A Roblox não publica cliente para Linux nem endossa o Sober — é um projeto
    comunitário. Até 05/2026 não há ondas de banimento reportadas por usá-lo, e
    o servidor enxerga você como um jogador Android. A decisão de usar é sua.

  Log desta execução: $LOG_FILE

EOF
}

# =============================================================================
#  MENU INTERATIVO (quando chamado sem argumentos)
# =============================================================================
cmd_menu() {
  detect_system
  printf '\n%b  ╔══════════════════════════════════════════════════╗%b\n' "$C_GRN" "$C_R"
  printf '%b  ║   ROBLOX no LINUX · instalador %-16s   ║%b\n' "$C_GRN" "$TOOL_VERSION" "$C_R"
  printf '%b  ╚══════════════════════════════════════════════════╝%b\n' "$C_GRN" "$C_R"
  printf '  %s · %s · %s núcleos\n' "${SYS[distro_name]}" "${SYS[arch]}" "${SYS[cores]}"
  if [ "${SYS[sober]}" = "1" ]; then
    printf '  %bStatus: Roblox instalado (%s)%b\n' "$C_GRN" "${SYS[sober_ver]:-?}" "$C_R"
  else
    printf '  %bStatus: Roblox ainda não instalado%b\n' "$C_YEL" "$C_R"
  fi
  cat <<EOF

  ${C_B}1)${C_R} Instalar o Roblox        ${C_DIM}(Flatpak + Flathub + drivers + Sober)${C_R}
  ${C_B}2)${C_R} Diagnóstico do sistema   ${C_DIM}(mostra o que falta e como corrigir)${C_R}
  ${C_B}3)${C_R} Abrir o Roblox
  ${C_B}4)${C_R} Atualizar                ${C_DIM}(use quando o jogo parar de abrir)${C_R}
  ${C_B}5)${C_R} Instalar o Studio        ${C_DIM}(criar seus próprios jogos · Vinegar)${C_R}
  ${C_B}6)${C_R} Simular a instalação     ${C_DIM}(--dry-run: não altera nada)${C_R}
  ${C_B}7)${C_R} Remover o Roblox
  ${C_B}0)${C_R} Sair

EOF
  printf '  Escolha: '
  local opt; read -r opt || opt=0
  case "$opt" in
    1) cmd_install ;;
    2) cmd_doctor ;;
    3) cmd_launch ;;
    4) cmd_update ;;
    5) cmd_studio ;;
    6) DRY_RUN=1; cmd_install ;;
    7) cmd_uninstall ;;
    *) say "Até logo!" ;;
  esac
}

# =============================================================================
#  PARSER DE ARGUMENTOS
# =============================================================================
COMMAND=""
while [ $# -gt 0 ]; do
  case "$1" in
    install|doctor|launch|update|studio|fastflags|uninstall|help) COMMAND="$1"; shift ;;
    -y|--yes)     ASSUME_YES=1; shift ;;
    -n|--dry-run) DRY_RUN=1; shift ;;
    --system)     USER_SCOPE=0; shift ;;
    --user)       USER_SCOPE=1; shift ;;
    --no-gpu)     INSTALL_GPU=0; shift ;;
    --studio)     INSTALL_STUDIO=1; shift ;;
    --no-open)    OPEN_AFTER=0; shift ;;
    -h|--help|--ajuda) COMMAND="help"; shift ;;
    -v|--version) printf 'roblox-linux %s\n' "$TOOL_VERSION"; exit 0 ;;
    *) printf 'Opção desconhecida: %s\n\n' "$1" >&2; cmd_help; exit 2 ;;
  esac
done

# =============================================================================
#  EXECUÇÃO
# =============================================================================
# `ROBLOX_LINUX_NO_RUN=1` permite carregar este arquivo para testes unitários
# (source) sem disparar nenhum comando de verdade.
if [ "${ROBLOX_LINUX_NO_RUN:-0}" = "1" ]; then
  return 0 2>/dev/null || true
fi

case "$COMMAND" in
  install)   cmd_install ;;
  doctor)    cmd_doctor ;;
  launch)    cmd_launch ;;
  update)    cmd_update ;;
  studio)    cmd_studio ;;
  fastflags) cmd_fastflags ;;
  uninstall) cmd_uninstall ;;
  help)      cmd_help ;;
  "")        cmd_menu ;;
esac
