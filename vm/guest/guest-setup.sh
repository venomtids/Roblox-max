#!/usr/bin/env bash
# Executado DENTRO do rootfs Debian bookworm (chroot) pelo vm/build-image.sh.
# Instala o desktop (XFCE, KasmVNC, Wine, Vinegar), o gateway e o serviço systemd.
set -Eeuo pipefail

KASMVNC_VERSION="${KASMVNC_VERSION:-1.5.0}"
VINEGAR_VERSION="${VINEGAR_VERSION:-v1.9.4}"
GO_VERSION="${GO_VERSION:-1.26.0}"
ARCH="$(dpkg --print-architecture)"

log() { printf '\n==> %s\n' "$*"; }

export DEBIAN_FRONTEND=noninteractive

# --- Pacotes base, kernel e boot ---------------------------------------------
log "Pacotes base, kernel e GRUB"
# Evita que o grub-pc pergunte o disco de instalação dentro do chroot (o GRUB é instalado pelo host).
echo 'grub-pc grub-pc/install_devices_empty boolean true' | debconf-set-selections
echo 'grub-pc grub-pc/install_devices multiselect' | debconf-set-selections
dpkg --add-architecture i386
apt-get update
apt-get install -y --no-install-recommends \
  linux-image-amd64 grub-pc systemd-sysv systemd-resolved \
  ca-certificates curl locales \
  xfce4 xfce4-terminal dbus-x11 xauth xfonts-base xdg-utils \
  fonts-noto-core fonts-dejavu-core \
  libgl1-mesa-dri libglx-mesa0 libegl-mesa0 mesa-utils \
  libvulkan1 mesa-vulkan-drivers vulkan-tools \
  wine64 wine32 winetricks cabextract \
  libgtk-4-1 libadwaita-1-0 libglib2.0-bin \
  nodejs npm

# --- Rede (DHCP via systemd-networkd, fornecido pelo QEMU user-mode) ----------
log "Configurando rede"
systemctl enable systemd-networkd systemd-resolved
cat > /etc/systemd/network/10-wired.network <<'EOF'
[Match]
Name=en* eth*

[Network]
DHCP=yes
EOF
ln -sf /run/systemd/resolve/resolv.conf /etc/resolv.conf

echo "roblox-linux" > /etc/hostname
cat > /etc/fstab <<'EOF'
LABEL=rootfs  /  ext4  errors=remount-ro  0  1
EOF

# --- Leitura do token pelo fw_cfg do QEMU -------------------------------------
echo qemu_fw_cfg > /etc/modules-load.d/qemu-fw-cfg.conf

# --- Console serial (log do QEMU) e GRUB --------------------------------------
sed -i 's/^GRUB_CMDLINE_LINUX=.*/GRUB_CMDLINE_LINUX="console=tty0 console=ttyS0,115200"/' /etc/default/grub
grep -q '^GRUB_TERMINAL' /etc/default/grub || echo 'GRUB_TERMINAL="console serial"' >> /etc/default/grub
echo 'GRUB_SERIAL_COMMAND="serial --unit=0 --speed=115200"' >> /etc/default/grub

# --- KasmVNC -------------------------------------------------------------------
log "Instalando KasmVNC ${KASMVNC_VERSION}"
curl -fsSL -o /tmp/kasmvncserver.deb \
  "https://github.com/kasmtech/KasmVNC/releases/download/v${KASMVNC_VERSION}/kasmvncserver_bookworm_${KASMVNC_VERSION}_${ARCH}.deb"
apt-get install -y --no-install-recommends /tmp/kasmvncserver.deb
rm -f /tmp/kasmvncserver.deb

# --- Vinegar (go.mod upstream exige Go 1.26) ----------------------------------
log "Compilando Vinegar ${VINEGAR_VERSION}"
apt-get install -y --no-install-recommends make pkg-config g++ libglib2.0-dev-bin gettext libvulkan-dev
case "$ARCH" in amd64) GO_ARCH=amd64 ;; arm64) GO_ARCH=arm64 ;; *) echo "arch $ARCH não suportada"; exit 1 ;; esac
curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${GO_ARCH}.tar.gz" | tar -xz -C /usr/local
export PATH="/usr/local/go/bin:$PATH"
SRC=/tmp/vinegar-src
rm -rf "$SRC" && mkdir -p "$SRC"
curl -fsSL "https://github.com/vinegarhq/vinegar/archive/refs/tags/${VINEGAR_VERSION}.tar.gz" \
  | tar -xz -C "$SRC" --strip-components=1
(cd "$SRC" && make all && make install PREFIX=/usr/local)
rm -rf "$SRC" /usr/local/go/pkg/mod /root/.cache

# --- Usuário do desktop --------------------------------------------------------
log "Criando usuário desktop"
useradd --create-home --uid 1000 --shell /bin/bash desktop
mkdir -p /etc/roblox-linux

# --- Aplicativo (gateway + frontend + entrypoint) -----------------------------
log "Instalando o aplicativo em /opt/roblox-linux"
mkdir -p /opt/roblox-linux
cp -r /tmp/app /opt/roblox-linux/app
chmod 0755 /opt/roblox-linux/app/entrypoint.sh
(cd /opt/roblox-linux/app && npm ci --omit=dev --no-audit --no-fund)
cp /opt/roblox-linux/app/config/vinegar/config.toml.template /etc/roblox-linux/vinegar-config.toml.template
chown -R desktop:desktop /opt/roblox-linux/app/node_modules 2>/dev/null || true

# --- Serviço systemd ----------------------------------------------------------
log "Instalando o serviço systemd"
install -m 0755 /tmp/guest/roblox-fetch-token /usr/local/sbin/roblox-fetch-token
install -m 0755 /tmp/guest/roblox-run /usr/local/sbin/roblox-run
install -m 0644 /tmp/guest/roblox-linux.service /etc/systemd/system/roblox-linux.service
systemctl enable roblox-linux.service

# --- Limpeza -----------------------------------------------------------------
apt-get clean
rm -rf /var/lib/apt/lists/* /tmp/app /tmp/guest
log "Configuração do guest concluída"
