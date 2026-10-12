#!/usr/bin/env bash
# Gera a imagem de VM "roblox-linux.qcow2" (Debian bookworm, BIOS/MBR, ext4).
#
# Onde rodar: um Linux com root (ou a própria distro WSL Debian):
#   sudo apt-get install -y debootstrap parted qemu-utils grub-pc-bin
#   sudo ./vm/build-image.sh
#
# Saída: vm-build/roblox-linux.qcow2  (copie para windows\image\ junto com o qemu)
set -Eeuo pipefail

[[ $EUID -eq 0 ]] || { echo "Execute como root (sudo)." >&2; exit 1; }

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SUITE="${SUITE:-bookworm}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"
IMG_SIZE="${IMG_SIZE:-16G}"
WORK="${WORK:-$ROOT_DIR/vm-build}"
ROOTFS="$WORK/rootfs"
RAW="$WORK/roblox-linux.raw"
OUT="$WORK/roblox-linux.qcow2"

for tool in debootstrap parted losetup mkfs.ext4 qemu-img grub-install chroot; do
  command -v "$tool" >/dev/null || { echo "Ferramenta ausente: $tool" >&2; exit 1; }
done

LOOP=""
MOUNTS=()

cleanup() {
  set +e
  for m in "${MOUNTS[@]}"; do umount -l "$m" 2>/dev/null; done
  umount -R "$ROOTFS" 2>/dev/null
  [[ -n "$LOOP" ]] && losetup -d "$LOOP" 2>/dev/null
}
trap cleanup EXIT

mkdir -p "$WORK"
rm -f "$RAW" "$OUT"

echo "==> Criando disco bruto de ${IMG_SIZE}"
truncate -s "$IMG_SIZE" "$RAW"
parted -s "$RAW" mklabel msdos mkpart primary ext4 1MiB 100% set 1 boot on

LOOP="$(losetup --find --show --partscan "$RAW")"
PART="${LOOP}p1"
for _ in $(seq 1 20); do [[ -b "$PART" ]] && break; sleep 0.5; done
[[ -b "$PART" ]] || { echo "Partição $PART não apareceu" >&2; exit 1; }

echo "==> Formatando e montando"
mkfs.ext4 -q -L rootfs "$PART"
mkdir -p "$ROOTFS"
mount "$PART" "$ROOTFS"

echo "==> debootstrap ${SUITE} (pode levar bastante tempo)"
debootstrap --variant=minbase --arch=amd64 \
  --include=systemd-sysv,ca-certificates \
  "$SUITE" "$ROOTFS" "$MIRROR"

echo "==> Preparando chroot"
for fs in proc sys dev dev/pts; do
  mkdir -p "$ROOTFS/$fs"
  mount --bind "/$fs" "$ROOTFS/$fs"
  MOUNTS+=("$ROOTFS/$fs")
done
cp /etc/resolv.conf "$ROOTFS/etc/resolv.conf"

rm -rf "$ROOTFS/tmp/app" "$ROOTFS/tmp/guest"
cp -r "$ROOT_DIR/dist/RobloxLinux/app" "$ROOTFS/tmp/app" 2>/dev/null || {
  # Sem dist/: usa a árvore do repositório (mesmos arquivos do pacote Windows).
  mkdir -p "$ROOTFS/tmp/app/wsl" "$ROOTFS/tmp/app/config/vinegar"
  cp "$ROOT_DIR/server.js" "$ROOT_DIR/package.json" "$ROOT_DIR/package-lock.json" \
     "$ROOT_DIR/entrypoint.sh" "$ROOTFS/tmp/app/"
  cp -r "$ROOT_DIR/docs" "$ROOTFS/tmp/app/docs"
  cp "$ROOT_DIR/config/vinegar/config.toml.template" "$ROOTFS/tmp/app/config/vinegar/"
}
mkdir -p "$ROOTFS/tmp/guest"
cp "$ROOT_DIR/vm/guest/"* "$ROOTFS/tmp/guest/"
cp "$ROOT_DIR/vm/guest/guest-setup.sh" "$ROOTFS/tmp/guest-setup.sh"
chmod 0755 "$ROOTFS/tmp/guest-setup.sh" "$ROOTFS/tmp/guest/roblox-run" "$ROOTFS/tmp/guest/roblox-fetch-token"

echo "==> Configurando o sistema dentro do chroot"
chroot "$ROOTFS" /bin/bash /tmp/guest-setup.sh

echo "==> Instalando GRUB (BIOS)"
chroot "$ROOTFS" grub-install --target=i386-pc --recheck "$LOOP"
chroot "$ROOTFS" update-grub

echo "==> Convertendo para qcow2 comprimido"
qemu-img convert -c -O qcow2 "$RAW" "$OUT"
rm -f "$RAW"

echo "==> Pronto: $OUT ($(du -h "$OUT" | cut -f1))"
