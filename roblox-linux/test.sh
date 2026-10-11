#!/usr/bin/env bash
# =============================================================================
#  TESTES DO ROBLOX-LINUX
# =============================================================================
#  Valida a LÓGICA do instalador sem instalar nada e sem tocar no sistema:
#  sintaxe, detecção de distro/gerenciador, hardware, planos de comando e
#  o contrato do --dry-run (garantia de que ele NÃO executa nada).
#
#  Uso:  ./test.sh      (ou: npm run test:linux)
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALLER="$SCRIPT_DIR/roblox-linux.sh"
PASS=0; FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

r='\033[0m'; g='\033[38;5;46m'; rd='\033[38;5;203m'; b='\033[1m'; d='\033[2m'

t() { # t "nome" comando...
  local name="$1"; shift
  if "$@" >/dev/null 2>&1; then printf "  ${g}PASS${r}  %s\n" "$name"; PASS=$((PASS+1));
  else printf "  ${rd}FAIL${r}  %s\n" "$name"; FAIL=$((FAIL+1)); fi
}
t_contains() { # t_contains "nome" "arquivo" "texto"
  if grep -qF -- "$3" "$2" 2>/dev/null; then printf "  ${g}PASS${r}  %s\n" "$1"; PASS=$((PASS+1));
  else printf "  ${rd}FAIL${r}  %s\n" "$1"; printf "        ${d}esperado conter: %s${r}\n" "$3"; FAIL=$((FAIL+1)); fi
}
t_missing() { # t_missing "nome" "arquivo" "texto"
  if grep -qF -- "$3" "$2" 2>/dev/null; then printf "  ${rd}FAIL${r}  %s\n" "$1"; FAIL=$((FAIL+1));
  else printf "  ${g}PASS${r}  %s\n" "$1"; PASS=$((PASS+1)); fi
}

printf "\n${b}ROBLOX-LINUX · testes do instalador${r}\n\n"

# ── 1. Estrutura e sintaxe ───────────────────────────────────────────────────
printf "${b}1. Estrutura${r}\n"
t "sintaxe bash válida"            bash -n "$INSTALLER"
t "tem shebang bash"               test -x "$INSTALLER"
t "--version responde"             bash -c "'$INSTALLER' --version | grep -q 'roblox-linux 1'"
t "--help lista os comandos"       bash -c "'$INSTALLER' --help | grep -q 'doctor'"
t "opção inválida retorna erro 2"  bash -c "'$INSTALLER' --inexistente >/dev/null 2>&1; [ \$? -eq 2 ]"
t "não usa curl | bash"            bash -c "! grep -qE 'curl .*\\| *(ba)?sh' '$INSTALLER'"

# ── 2. Contrato do --dry-run: NÃO executa nada ───────────────────────────────
printf "\n${b}2. Segurança do --dry-run${r}\n"
SENTINEL="$TMP/nao-devia-existir"
"$INSTALLER" --dry-run -y install >"$TMP/plan.txt" 2>&1 || true
t_contains "plano mostra os passos"        "$TMP/plan.txt" "1/5 · Flatpak"
t_contains "plano mostra o comando real"   "$TMP/plan.txt" "[dry-run]"
t_contains "plano instala o Sober"         "$TMP/plan.txt" "org.vinegarhq.Sober"
t_contains "plano configura o Flathub"     "$TMP/plan.txt" "flathub"
t "nenhum arquivo foi criado no HOME"      bash -c "[ ! -e '$SENTINEL' ] && [ ! -f \"\$HOME/.local/share/applications/roblox-linux.desktop\" ] || true"
t "dry-run termina com sucesso"            bash -c "'$INSTALLER' --dry-run -y --no-gpu install >/dev/null 2>&1"

# ── 3. Detecção de sistema ───────────────────────────────────────────────────
printf "\n${b}3. Detecção de sistema${r}\n"
SYSINFO="$TMP/sys.txt"
"$INSTALLER" doctor >"$SYSINFO" 2>&1 || true
t_contains "detecta a distro real"         "$SYSINFO" "$( (. /etc/os-release; printf '%s' "$PRETTY_NAME") )"
t_contains "checa arquitetura"             "$SYSINFO" "$(uname -m)"
t_contains "checa kernel"                  "$SYSINFO" "$(uname -r)"
t_contains "checa flatpak"                 "$SYSINFO" "Flatpak"
t_contains "checa o estado do Roblox"      "$SYSINFO" "Sober"
t_contains "checa drivers de vídeo"        "$SYSINFO" "Vulkan"
t "doctor termina com código 0"            bash -c "'$INSTALLER' doctor >/dev/null 2>&1"

# ── 4. Unidades internas (source com guarda) ─────────────────────────────────
printf "\n${b}4. Unidades internas${r}\n"
cat >"$TMP/units.sh" <<UNIT
set -uo pipefail
export ROBLOX_LINUX_NO_RUN=1
# impede que o script faça trap de ERR durante os testes
trap - ERR
. "$INSTALLER"

fail() { echo "  \$1"; exit 1; }

# pkg_cmd_for deve mapear cada família de gerenciador
SYS[pkg]=apt;    [ "\$(pkg_cmd_for pk-apt pk-dnf pk-pac pk-zyp)" = "sudo apt install pk-apt" ]      || fail "apt"
SYS[pkg]=dnf;    [ "\$(pkg_cmd_for pk-apt pk-dnf pk-pac pk-zyp)" = "sudo dnf install pk-dnf" ]      || fail "dnf"
SYS[pkg]=pacman; [ "\$(pkg_cmd_for pk-apt pk-dnf pk-pac pk-zyp)" = "sudo pacman -S pk-pac" ]        || fail "pacman"
SYS[pkg]=zypper; [ "\$(pkg_cmd_for pk-apt pk-dnf pk-pac pk-zyp)" = "sudo zypper install pk-zyp" ]   || fail "zypper"
SYS[pkg]=desconhecido; pkg_cmd_for x y z w | grep -q "gerenciador"                                 || fail "fallback"

# dry-run: run() não executa; arquivo de teste não deve existir
DRY_RUN=1; run touch "$TMP/SENTINELA"; [ ! -e "$TMP/SENTINELA" ] || fail "run() executou em dry-run"
DRY_RUN=0; run touch "$TMP/executou";  [ -e "$TMP/executou" ]    || fail "run() não executou fora do dry-run"

# ask() com --yes nunca pergunta
ASSUME_YES=1; ask "teste" || fail "ask com yes"
ASSUME_YES=0; ask "teste" </dev/null || fail "ask sem tty"
echo "UNIDADES_OK"
UNIT
if bash "$TMP/units.sh" >"$TMP/units.log" 2>&1; then
  t "unidades internas (pkg_cmd_for + run + ask)" grep -q "UNIDADES_OK" "$TMP/units.log"
else
  printf "  ${rd}FAIL${r}  unidades internas\n"; sed 's/^/        /' "$TMP/units.log"; FAIL=$((FAIL+1))
fi

# ── 5. Bloqueios de hardware (simulados) ─────────────────────────────────────
printf "\n${b}5. Bloqueios de hardware (simulados)${r}\n"
cat >"$TMP/blocks.sh" <<BLOCK
export ROBLOX_LINUX_NO_RUN=1
trap - ERR
. "$INSTALLER"
ASSUME_YES=1

# x86_64 + SSE4.2 → passa
preflight x86_64 1 1 0 || { echo "FALHOU: recusou um sistema válido"; exit 1; }
# CPU sem SSE4.1 → recusa com explicação
if preflight x86_64 0 0 0 >"$TMP/b1" 2>&1; then echo "FALHOU: aceitou CPU sem SSE4.1"; exit 1; fi
grep -q "SSE4.1" "$TMP/b1" || { echo "FALHOU: mensagem SSE4.1 ausente"; exit 1; }
# ARM → recusa com "arquitetura"
if preflight aarch64 1 1 0 >"$TMP/b2" 2>&1; then echo "FALHOU: aceitou ARM"; exit 1; fi
grep -qi "arquitetura" "$TMP/b2" || { echo "FALHOU: mensagem de arquitetura ausente"; exit 1; }
# SSE4.1 sem SSE4.2 → aceita, mas avisa
preflight x86_64 1 0 0 >"$TMP/b3" 2>&1 || { echo "FALHOU: recusou SSE4.1"; exit 1; }
grep -q "SSE4.2" "$TMP/b3" || { echo "FALHOU: aviso de emulação ausente"; exit 1; }
# container → pergunta (com --yes segue)
preflight x86_64 1 1 1 || { echo "FALHOU: --yes deveria seguir em container"; exit 1; }
echo "BLOCOS_OK"
BLOCK
if bash "$TMP/blocks.sh" >"$TMP/blocks.log" 2>&1; then
  t "5 cenários de preflight (SSE4.1/ARM/emulação/container)" grep -q "BLOCOS_OK" "$TMP/blocks.log"
else
  printf "  ${rd}FAIL${r}  bloqueios de hardware\n"; sed 's/^/        /' "$TMP/blocks.log"; FAIL=$((FAIL+1))
fi

# ── 6. Conteúdo técnico obrigatório ──────────────────────────────────────────
printf "\n${b}6. Conteúdo e precisão técnica${r}\n"
t_contains "usa o Flatpak correto do Sober"   "$INSTALLER" "org.vinegarhq.Sober"
t_contains "usa o Flathub correto"            "$INSTALLER" "dl.flathub.org/repo/flathub.flatpakrepo"
t_contains "conhece o Vinegar (Studio)"       "$INSTALLER" "org.vinegarhq.Vinegar"
t_contains "avisa que Wine está morto"        "$INSTALLER" "anti-cheat Hyperion é Windows-only"
t_contains "trata distro imutável"            "$INSTALLER" "ostree-booted"
t_contains "avisa risco de FastFlags"         "$INSTALLER" "risco de banimento"
t_contains "recomenda SSE4.2"                 "$INSTALLER" "SSE4.2"
t_missing  "não promete contornar anti-cheat" "$INSTALLER" "bypass"
t_missing  "não baixa binário de terceiros"   "$INSTALLER" "curl -L http"

# ── Resultado ────────────────────────────────────────────────────────────────
printf "\n${b}Resultado:${r} "
if [ "$FAIL" -eq 0 ]; then printf "${g}%d testes, tudo verde${r}\n\n" "$PASS"; exit 0
else printf "${rd}%d falhas${r} de %d testes\n\n" "$FAIL" "$((PASS+FAIL))"; exit 1; fi
