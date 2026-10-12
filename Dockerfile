# syntax=docker/dockerfile:1.7
#
# Cloud Linux Desktop — Debian bookworm slim + XFCE4 + KasmVNC + Mesa + Wine + Vinegar.
# Publicado como um único serviço (Render, Railway ou VPS). Porta pública: $PORT (padrão 8080).
#
# Build local:  docker build -t cloud-desktop .
# Run local:    docker run --rm -p 8080:8080 -e ACCESS_TOKEN=troque-por-um-token-de-16+-caracteres cloud-desktop

ARG VINEGAR_VERSION=v1.9.4
ARG KASMVNC_VERSION=1.5.0

# ---------------------------------------------------------------------------
# Estágio 1: compilar o Vinegar (go.mod upstream exige Go 1.26)
# ---------------------------------------------------------------------------
FROM golang:1.26-bookworm AS vinegar-build
ARG VINEGAR_VERSION
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl make pkg-config g++ \
      libglib2.0-dev-bin gettext libvulkan-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN curl -fsSL "https://github.com/vinegarhq/vinegar/archive/refs/tags/${VINEGAR_VERSION}.tar.gz" \
  | tar -xz --strip-components=1

RUN make all \
 && make install DESTDIR=/out PREFIX=/usr

# ---------------------------------------------------------------------------
# Estágio 2: runtime (Node 22 sobre Debian bookworm slim)
# ---------------------------------------------------------------------------
FROM node:22-bookworm-slim AS runtime
ARG KASMVNC_VERSION
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    TZ=UTC

# i386 é necessário para o Wine 32-bit (componentes de launcher/DirectX de 32 bits).
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl tini \
      xfce4 xfce4-terminal dbus-x11 xauth xfonts-base xdg-utils \
      fonts-noto-core fonts-dejavu-core \
      libgl1-mesa-dri libglx-mesa0 libegl-mesa0 mesa-utils \
      libvulkan1 mesa-vulkan-drivers vulkan-tools \
      wine64 wine32 winetricks cabextract \
      libgtk-4-1 libadwaita-1-0 libglib2.0-bin \
 && rm -rf /var/lib/apt/lists/*

# KasmVNC: pacote oficial da release (Debian bookworm, amd64 ou arm64).
RUN case "${TARGETARCH}" in \
      amd64|arm64) ;; \
      *) echo "Arquitetura não suportada pelo KasmVNC: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
 && curl -fsSL -o /tmp/kasmvncserver.deb \
      "https://github.com/kasmtech/KasmVNC/releases/download/v${KASMVNC_VERSION}/kasmvncserver_bookworm_${KASMVNC_VERSION}_${TARGETARCH}.deb" \
 && apt-get update \
 && apt-get install -y --no-install-recommends /tmp/kasmvncserver.deb \
 && rm -rf /tmp/kasmvncserver.deb /var/lib/apt/lists/*

# Vinegar compilado no estágio anterior.
COPY --from=vinegar-build /out/usr/ /usr/

# Usuário não-root para o desktop (o usuário "node" da imagem base é substituído).
RUN userdel -r node 2>/dev/null || true \
 && useradd --create-home --uid 1000 --shell /bin/bash desktop \
 && mkdir -p /opt/cloud-desktop /etc/cloud-desktop \
 && chown -R desktop:desktop /home/desktop

WORKDIR /opt/cloud-desktop
COPY package.json package-lock.json ./
RUN npm ci --omit=dev \
 && npm cache clean --force

COPY server.js ./
COPY config/vinegar/config.toml.template /etc/cloud-desktop/vinegar-config.toml.template
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod 0755 /usr/local/bin/entrypoint.sh \
 && chmod 0644 /etc/cloud-desktop/vinegar-config.toml.template

ENV PORT=8080 \
    DISPLAY=:1 \
    VNC_PORT=6901 \
    VNC_RESOLUTION=1920x1080 \
    VNC_DEPTH=24 \
    VNC_DESKTOP=xfce \
    ROBLOX_AUTOSTART=0 \
    ROBLOX_LAUNCH_CMD=vinegar \
    HOME=/home/desktop

USER desktop
WORKDIR /home/desktop

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=90s --retries=3 \
  CMD curl -fsS "http://127.0.0.1:${PORT}/health" >/dev/null || exit 1

# tini: PID 1 correto (repasse de sinais e remoção de processos zumbis do XFCE/Wine).
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/entrypoint.sh"]
