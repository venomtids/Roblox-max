#!/usr/bin/env bash
# Gera o pacote Windows: RobloxLinux.exe + app/ (gateway, frontend, scripts WSL).
#
# Requisitos (no Linux de build):  python3 com o pacote "ziglang" (cross-compiler C -> Windows)
#   python3 -m venv ~/zigenv && ~/zigenv/bin/pip install ziglang
#   ZIG="$HOME/zigenv/bin/python -m ziglang" ./windows/build.sh
#
# Saída: dist/RobloxLinux/ e dist/RobloxLinux-windows.zip
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZIG="${ZIG:-python3 -m ziglang}"
OUT="$ROOT/dist/RobloxLinux"

echo "==> Compilando RobloxLinux.exe (x86_64, sem dependências de runtime)"
mkdir -p "$ROOT/dist"
rm -rf "$OUT" && mkdir -p "$OUT/app"
# shellcheck disable=SC2086
$ZIG cc -target x86_64-windows-gnu -Os -s -Wall -Wextra -Werror -Wno-unused-parameter \
  "$ROOT/windows/RobloxLinux.c" -o "$OUT/RobloxLinux.exe" -luser32 -lshell32

echo "==> Montando app/"
cp "$ROOT/server.js" "$ROOT/package.json" "$ROOT/package-lock.json" "$ROOT/entrypoint.sh" "$OUT/app/"
cp -r "$ROOT/docs" "$OUT/app/docs"
mkdir -p "$OUT/app/config/vinegar" "$OUT/app/wsl"
cp "$ROOT/config/vinegar/config.toml.template" "$OUT/app/config/vinegar/"
cp "$ROOT/wsl/"*.sh "$OUT/app/wsl/"

cat > "$OUT/LEIA-ME.txt" <<'EOF'
Roblox Linux para Windows (WSL2)
================================

Requisitos: Windows 10 22H2+ ou Windows 11, virtualização habilitada na BIOS,
pelo menos 20 GB livres e 8 GB de RAM (recomendado 16 GB).

1. Abra o PowerShell como administrador nesta pasta e rode:
     .\RobloxLinux.exe install
   Se o WSL ainda não estiver ativo, o instalador pede uma reinicialização.
   Depois de reiniciar, rode o comando de novo.
   Na primeira vez, crie seu usuário Linux na janela da distro Debian.

2. Para abrir o desktop:
     .\RobloxLinux.exe start
   O navegador abre http://localhost:8080 já conectado.

3. Para encerrar:
     .\RobloxLinux.exe stop

Outros comandos: status, uninstall.
Os dados ficam dentro da distro "Debian" do WSL.

Observação: o Roblox oficial não tem cliente Linux. O caminho aqui usa Wine e o
Vinegar; o Vinegar, no upstream, é um bootstrapper voltado ao Roblox Studio.
EOF

echo "==> Gerando zip"
rm -f "$ROOT/dist/RobloxLinux-windows.zip"
(cd "$ROOT/dist" && python3 -m zipfile -c RobloxLinux-windows.zip RobloxLinux)

echo "==> Pronto"
ls -la "$ROOT/dist/RobloxLinux-windows.zip" "$OUT/RobloxLinux.exe"
