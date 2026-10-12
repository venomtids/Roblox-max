/**
 * Cloud Linux Desktop — cliente do frontend estático (GitHub Pages).
 *
 * Fluxo:
 *  1. Normaliza o endereço informado (http(s)/ws(s), host ou IP:porta).
 *  2. Valida o token em POST {backend}/api/auth/verify.
 *  3. Abre o cliente noVNC (RFB) em wss://{backend}/websockify?token=...
 *  4. Reconecta automaticamente em quedas inesperadas (backoff exponencial).
 */

import RFB from './vendor/novnc/core/rfb.js';

const STORAGE_KEY = 'cloud-desktop:server-url';
const MAX_RECONNECT_ATTEMPTS = 5;
const CONNECT_TIMEOUT_MS = 20000;

const el = {
  body: document.body,
  form: document.getElementById('connect-form'),
  serverUrl: document.getElementById('server-url'),
  token: document.getElementById('access-token'),
  connectBtn: document.getElementById('btn-connect'),
  disconnectBtn: document.getElementById('btn-disconnect'),
  cadBtn: document.getElementById('btn-cad'),
  fullscreenBtn: document.getElementById('btn-fullscreen'),
  status: document.getElementById('status'),
  badge: document.getElementById('state-badge'),
  screen: document.getElementById('screen'),
  stage: document.getElementById('stage'),
  optScale: document.getElementById('opt-scale'),
  optViewOnly: document.getElementById('opt-viewonly'),
  optKeyLock: document.getElementById('opt-keylock'),
  optQuality: document.getElementById('opt-quality'),
};

const session = {
  rfb: null,
  state: 'idle', // idle | connecting | connected | reconnecting | error
  wsUrl: null,
  httpBase: null,
  manualStop: false,
  reconnectAttempts: 0,
  reconnectTimer: null,
  connectTimer: null,
};

const STATE_LABELS = {
  idle: 'Desconectado',
  connecting: 'Conectando…',
  connected: 'Conectado',
  reconnecting: 'Reconectando…',
  error: 'Erro',
};

// ---------------------------------------------------------------------------
// Utilidades de UI
// ---------------------------------------------------------------------------
function setStatus(message, kind = '') {
  el.status.textContent = message;
  el.status.className = kind;
}

function setState(state) {
  session.state = state;
  el.body.dataset.state = state;
  el.badge.textContent = STATE_LABELS[state] ?? state;
  updateControls();
}

function updateControls() {
  const busy = session.state === 'connecting' || session.state === 'reconnecting';
  const live = session.state === 'connected';
  el.connectBtn.disabled = busy || live;
  el.disconnectBtn.disabled = !(busy || live);
  el.cadBtn.disabled = !live;
  el.fullscreenBtn.disabled = !live;
  el.serverUrl.disabled = busy || live;
  el.token.disabled = busy || live;
}

// ---------------------------------------------------------------------------
// Normalização do endereço do backend
// ---------------------------------------------------------------------------
/**
 * Aceita:  https://app.onrender.com | app.onrender.com | 203.0.113.10:8080 | wss://host/websockify
 * Retorna: { httpBase, wsBase } onde wsBase não contém "/websockify".
 */
export function normalizeServerUrl(raw) {
  let input = String(raw || '').trim();
  if (!input) {
    throw new Error('Informe o endereço do servidor de backend.');
  }

  const pageIsHttps = window.location.protocol === 'https:';
  if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(input)) {
    input = `${pageIsHttps ? 'https' : 'http'}://${input}`;
  }

  let url;
  try {
    url = new URL(input);
  } catch {
    throw new Error('Endereço inválido. Exemplo: https://seu-servico.onrender.com');
  }

  if (!['http:', 'https:', 'ws:', 'wss:'].includes(url.protocol)) {
    throw new Error('Use um endereço http(s):// ou ws(s):// .');
  }

  const secure = url.protocol === 'https:' || url.protocol === 'wss:';
  if (pageIsHttps && !secure) {
    throw new Error(
      'Esta página é HTTPS: use https:// ou wss:// no endereço do backend. ' +
        'Navegadores bloqueiam ws:// a partir de páginas HTTPS (mixed content).'
    );
  }

  const basePath = url.pathname.replace(/\/+$/, '').replace(/\/websockify$/, '');
  const host = url.host;
  return {
    httpBase: `${secure ? 'https' : 'http'}://${host}${basePath}`,
    wsBase: `${secure ? 'wss' : 'ws'}://${host}${basePath}`,
  };
}

async function verifyToken(httpBase, token) {
  let response;
  try {
    response = await fetch(`${httpBase}/api/auth/verify`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ token }),
      cache: 'no-store',
      credentials: 'omit',
      mode: 'cors',
    });
  } catch {
    throw new Error(
      'Não foi possível alcançar o backend. Verifique a URL, se o serviço está ativo ' +
        'e se ALLOWED_ORIGINS inclui a origem desta página.'
    );
  }
  if (response.status === 401) throw new Error('Token inválido.');
  if (response.status === 403) throw new Error('Origem não permitida pelo backend (ALLOWED_ORIGINS).');
  if (response.status === 429) throw new Error('Muitas tentativas falhas. Aguarde alguns minutos.');
  if (!response.ok) throw new Error(`Backend respondeu com status ${response.status}.`);
}

// ---------------------------------------------------------------------------
// Sessão noVNC
// ---------------------------------------------------------------------------
function applySettings(rfb) {
  const scale = el.optScale.checked;
  rfb.scaleViewport = scale;
  rfb.resizeSession = false;      // o desktop mantém a resolução do servidor
  rfb.clipViewport = !scale;      // sem escala: rolagem/arraste dentro da tela
  rfb.dragViewport = false;
  rfb.viewOnly = el.optViewOnly.checked;
  rfb.focusOnClick = true;        // clique dá foco ao teclado remoto
  rfb.showDotCursor = true;       // mostra cursor quando o servidor não envia um
  rfb.qualityLevel = Number(el.optQuality.value);
  rfb.compressionLevel = 2;
}

function openSession(wsUrl) {
  clearTimeout(session.connectTimer);
  if (session.rfb) {
    teardownRfb();
  }

  let rfb;
  try {
    rfb = new RFB(el.screen, wsUrl, { shared: true });
  } catch (err) {
    fail(`Falha ao iniciar o cliente: ${err.message}`);
    return;
  }

  session.rfb = rfb;
  applySettings(rfb);

  session.connectTimer = setTimeout(() => {
    if (session.rfb === rfb && session.state !== 'connected') {
      setStatus('Tempo esgotado ao conectar ao desktop.', 'error');
      session.manualStop = true;
      teardownRfb();
      setState('error');
    }
  }, CONNECT_TIMEOUT_MS);

  // Ignora eventos de instâncias antigas (evita que uma sessão substituída zere a nova).
  const isCurrent = () => session.rfb === rfb;

  rfb.addEventListener('connect', () => {
    if (!isCurrent()) return;
    clearTimeout(session.connectTimer);
    session.reconnectAttempts = 0;
    setState('connected');
    setStatus('Sessão ativa. Clique na tela para capturar teclado e mouse.', 'ok');
    rfb.focus();
  });

  rfb.addEventListener('disconnect', (event) => {
    if (!isCurrent()) return;
    clearTimeout(session.connectTimer);
    const clean = event.detail?.clean === true;
    session.rfb = null;
    if (session.manualStop) {
      setState('idle');
      setStatus('Desconectado.');
      return;
    }
    if (clean) {
      setState('idle');
      setStatus('Sessão encerrada pelo servidor.');
      return;
    }
    scheduleReconnect();
  });

  rfb.addEventListener('securityfailure', (event) => {
    if (!isCurrent()) return;
    session.manualStop = true;
    const reason = event.detail?.reason || 'falha de segurança';
    fail(`Autenticação recusada pelo desktop: ${reason}`);
  });

  rfb.addEventListener('desktopname', (event) => {
    if (!isCurrent()) return;
    if (event.detail?.name) document.title = `${event.detail.name} · Cloud Linux Desktop`;
  });
}

function teardownRfb() {
  const rfb = session.rfb;
  session.rfb = null;
  if (rfb) {
    try {
      rfb.disconnect();
    } catch {
      /* já desconectado */
    }
  }
}

function scheduleReconnect() {
  if (session.reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
    fail('Conexão perdida e limite de reconexões atingido. Clique em Conectar novamente.');
    return;
  }
  session.reconnectAttempts += 1;
  const delay = Math.min(30000, 1000 * 2 ** (session.reconnectAttempts - 1));
  setState('reconnecting');
  setStatus(`Conexão perdida. Nova tentativa ${session.reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS} em ${Math.round(delay / 1000)}s…`, 'warn');
  clearTimeout(session.reconnectTimer);
  session.reconnectTimer = setTimeout(() => {
    if (session.manualStop || !session.wsUrl) return;
    openSession(session.wsUrl);
  }, delay);
}

function fail(message) {
  clearTimeout(session.reconnectTimer);
  setState('error');
  setStatus(message, 'error');
}

function stopSession() {
  session.manualStop = true;
  clearTimeout(session.reconnectTimer);
  clearTimeout(session.connectTimer);
  teardownRfb();
  session.wsUrl = null;
  session.reconnectAttempts = 0;
  setState('idle');
  setStatus('Desconectado.');
}

// ---------------------------------------------------------------------------
// Ações
// ---------------------------------------------------------------------------
async function connect(event) {
  event.preventDefault();
  const token = el.token.value;
  if (!token) {
    setStatus('Informe o token de acesso.', 'error');
    return;
  }

  let target;
  try {
    target = normalizeServerUrl(el.serverUrl.value);
  } catch (err) {
    fail(err.message);
    return;
  }

  localStorage.setItem(STORAGE_KEY, el.serverUrl.value.trim());
  session.manualStop = false;
  session.reconnectAttempts = 0;
  setState('connecting');
  setStatus('Validando token…');

  try {
    await verifyToken(target.httpBase, token);
  } catch (err) {
    fail(err.message);
    return;
  }

  if (session.manualStop) return; // usuário cancelou durante a validação

  session.httpBase = target.httpBase;
  session.wsUrl = `${target.wsBase}/websockify?token=${encodeURIComponent(token)}`;
  setStatus('Abrindo canal WebSocket…');
  openSession(session.wsUrl);
}

function toggleFullscreen() {
  if (document.fullscreenElement) {
    document.exitFullscreen().catch(() => {});
  } else {
    el.stage.requestFullscreen().catch((err) => {
      setStatus(`Tela cheia indisponível: ${err.message}`, 'warn');
    });
  }
}

async function syncKeyboardLock() {
  if (!navigator.keyboard || typeof navigator.keyboard.lock !== 'function') return;
  try {
    if (document.fullscreenElement && el.optKeyLock.checked) {
      // Teclas do sistema (Windows/Meta) passam para o desktop remoto.
      await navigator.keyboard.lock(['MetaLeft', 'MetaRight', 'Tab']);
    } else {
      navigator.keyboard.unlock();
    }
  } catch {
    /* navegador sem suporte ou sem permissão: segue sem captura extra */
  }
}

// ---------------------------------------------------------------------------
// Eventos
// ---------------------------------------------------------------------------
el.form.addEventListener('submit', connect);
el.disconnectBtn.addEventListener('click', stopSession);
el.cadBtn.addEventListener('click', () => session.rfb && session.rfb.sendCtrlAltDel());
el.fullscreenBtn.addEventListener('click', toggleFullscreen);

for (const control of [el.optScale, el.optViewOnly, el.optQuality]) {
  control.addEventListener('change', () => session.rfb && applySettings(session.rfb));
}
el.optKeyLock.addEventListener('change', syncKeyboardLock);

document.addEventListener('fullscreenchange', () => {
  el.fullscreenBtn.textContent = document.fullscreenElement ? 'Sair da tela cheia' : 'Tela cheia';
  syncKeyboardLock();
  if (session.rfb) session.rfb.focus();
});

// Captura otimizada: sem menu de contexto sobre o desktop remoto (botão direito vai ao servidor).
el.screen.addEventListener('contextmenu', (event) => event.preventDefault());

window.addEventListener('beforeunload', () => {
  if (session.rfb) teardownRfb();
});

// ---------------------------------------------------------------------------
// Inicialização
// ---------------------------------------------------------------------------
(function init() {
  const saved = localStorage.getItem(STORAGE_KEY);
  if (saved) el.serverUrl.value = saved;
  if (!('keyboard' in navigator)) {
    el.optKeyLock.disabled = true;
    el.optKeyLock.parentElement.title = 'Keyboard Lock disponível apenas em navegadores Chromium';
  }
  setState('idle');

  // Modo local (RobloxLinux.exe): abre http://localhost:8080/#token=... e conecta automaticamente.
  // O fragmento (#) nunca é enviado ao servidor, e o auto-connect só ocorre em localhost.
  const hashParams = new URLSearchParams(window.location.hash.slice(1));
  const localToken = hashParams.get('token');
  if (localToken) {
    history.replaceState(null, '', window.location.pathname + window.location.search);
    if (['localhost', '127.0.0.1'].includes(window.location.hostname)) {
      el.serverUrl.value = window.location.origin;
      el.token.value = localToken;
      el.form.requestSubmit();
    }
  }
})();
