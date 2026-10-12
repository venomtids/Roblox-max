'use strict';

/**
 * Cloud Desktop Gateway
 *
 * Responsabilidades:
 *  - Ponto de entrada público único (Render / Railway / VPS) na porta $PORT.
 *  - Autenticação por token (ACCESS_TOKEN) para API e para o WebSocket do desktop.
 *  - Proxy WebSocket  /websockify  ->  KasmVNC em 127.0.0.1:$VNC_PORT.
 *  - CORS restrito a ALLOWED_ORIGINS (ex.: https://usuario.github.io).
 *  - Rate limit de tentativas de autenticação falhas por IP.
 *  - Endpoints de saúde e status para a plataforma de hospedagem.
 */

const http = require('node:http');
const net = require('node:net');
const crypto = require('node:crypto');
const express = require('express');
const httpProxy = require('http-proxy');
const pkg = require('./package.json');

// ---------------------------------------------------------------------------
// Configuração
// ---------------------------------------------------------------------------
const PORT = Number(process.env.PORT) || 8080;
const HOST = process.env.HOST || '0.0.0.0';
const ACCESS_TOKEN = process.env.ACCESS_TOKEN || '';
const VNC_HOST = process.env.VNC_HOST || '127.0.0.1';
const VNC_PORT = Number(process.env.VNC_PORT) || 6901;
const WS_PATH = '/websockify';
// STATIC_DIR: serve o frontend (docs/) pelo próprio gateway. Usado no modo local (Windows/WSL),
// onde a página e o WebSocket compartilham a mesma origem.
const STATIC_DIR = process.env.STATIC_DIR || '';
const ALLOWED_ORIGINS = (process.env.ALLOWED_ORIGINS || '')
  .split(',')
  .map((s) => s.trim().replace(/\/+$/, ''))
  .filter(Boolean);

const RATE_WINDOW_MS = 15 * 60 * 1000;
const RATE_MAX_FAILURES = 10;
const STARTED_AT = Date.now();

if (ACCESS_TOKEN.length < 16) {
  console.error(
    '[gateway] ACCESS_TOKEN ausente ou com menos de 16 caracteres. Defina um token forte no painel da plataforma.'
  );
  process.exit(1);
}

if (ALLOWED_ORIGINS.length === 0) {
  console.warn(
    '[gateway] ALLOWED_ORIGINS vazio: chamadas cross-origin do navegador serão recusadas. ' +
      'Defina, por exemplo, https://SEU_USUARIO.github.io'
  );
}

const sha256 = (value) => crypto.createHash('sha256').update(value, 'utf8').digest();
const TOKEN_DIGEST = sha256(ACCESS_TOKEN);

function tokenIsValid(candidate) {
  if (typeof candidate !== 'string' || candidate.length === 0 || candidate.length > 512) {
    return false;
  }
  // Compara digests de tamanho fixo para não vazar o tamanho do token por timing.
  return crypto.timingSafeEqual(sha256(candidate), TOKEN_DIGEST);
}

// ---------------------------------------------------------------------------
// Rate limit de falhas de autenticação (memória local, por IP)
// ---------------------------------------------------------------------------
const failures = new Map(); // ip -> { count, windowStart }

function clientIp(req) {
  const xff = req.headers['x-forwarded-for'];
  if (typeof xff === 'string' && xff.length > 0) {
    return xff.split(',')[0].trim();
  }
  return req.socket.remoteAddress || 'unknown';
}

function isBlocked(ip) {
  const entry = failures.get(ip);
  if (!entry) return false;
  if (Date.now() - entry.windowStart > RATE_WINDOW_MS) {
    failures.delete(ip);
    return false;
  }
  return entry.count >= RATE_MAX_FAILURES;
}

function recordFailure(ip) {
  const now = Date.now();
  const entry = failures.get(ip);
  if (!entry || now - entry.windowStart > RATE_WINDOW_MS) {
    failures.set(ip, { count: 1, windowStart: now });
  } else {
    entry.count += 1;
  }
}

setInterval(() => {
  const now = Date.now();
  for (const [ip, entry] of failures) {
    if (now - entry.windowStart > RATE_WINDOW_MS) failures.delete(ip);
  }
}, 60 * 1000).unref();

// ---------------------------------------------------------------------------
// Verificações de origem e do desktop
// ---------------------------------------------------------------------------
function originAllowed(origin, host, forwardedProto) {
  if (!origin) return true; // clientes não-navegador (ex.: curl) — o token ainda é exigido
  const clean = origin.replace(/\/+$/, '');
  if (ALLOWED_ORIGINS.includes(clean)) return true;
  // Mesma origem (página servida pelo próprio gateway, ex.: modo local).
  if (host) {
    const scheme = (forwardedProto || 'http').split(',')[0].trim();
    if (clean === `${scheme}://${host}`) return true;
  }
  return false;
}

function checkVnc(timeoutMs = 1000) {
  return new Promise((resolve) => {
    const socket = net.createConnection({ host: VNC_HOST, port: VNC_PORT });
    const done = (ok) => {
      socket.destroy();
      resolve(ok);
    };
    socket.setTimeout(timeoutMs);
    socket.once('connect', () => done(true));
    socket.once('timeout', () => done(false));
    socket.once('error', () => done(false));
  });
}

// ---------------------------------------------------------------------------
// HTTP (API)
// ---------------------------------------------------------------------------
const app = express();
app.disable('x-powered-by');
app.set('trust proxy', 1); // Render / Railway ficam atrás de proxy reverso
app.use(express.json({ limit: '1kb' }));

const originOk = (req) =>
  originAllowed(req.headers.origin, req.headers.host, req.headers['x-forwarded-proto']);

app.use((req, res, next) => {
  const origin = req.headers.origin;
  if (origin && originOk(req)) {
    res.setHeader('Access-Control-Allow-Origin', origin.replace(/\/+$/, ''));
    res.setHeader('Vary', 'Origin');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');
    res.setHeader('Access-Control-Max-Age', '600');
  }
  res.setHeader('Cache-Control', 'no-store');
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('Referrer-Policy', 'no-referrer');
  if (req.method === 'OPTIONS') {
    res.status(origin && originOk(req) ? 204 : 403).end();
    return;
  }
  next();
});

app.use((req, res, next) => {
  const origin = req.headers.origin;
  if (origin && !originOk(req)) {
    res.status(403).json({ error: 'origin_not_allowed' });
    return;
  }
  next();
});

if (STATIC_DIR) {
  app.use(express.static(STATIC_DIR, { index: 'index.html', dotfiles: 'ignore', maxAge: 0 }));
}

// Saúde pública (usada pelo health check da plataforma e pelo Dockerfile)
app.get('/health', async (_req, res) => {
  const desktop = (await checkVnc()) ? 'up' : 'down';
  res.status(200).json({ status: 'ok', desktop });
});

// Valida token enviado no corpo JSON (usado pelo frontend antes de abrir o WebSocket)
app.post('/api/auth/verify', (req, res) => {
  const ip = clientIp(req);
  if (isBlocked(ip)) {
    res.status(429).json({ error: 'too_many_attempts' });
    return;
  }
  const token = req.body && typeof req.body.token === 'string' ? req.body.token : '';
  if (!tokenIsValid(token)) {
    recordFailure(ip);
    res.status(401).json({ error: 'invalid_token' });
    return;
  }
  res.json({ ok: true, wsPath: WS_PATH });
});

// Status autenticado (Authorization: Bearer <token>)
app.get('/api/status', async (req, res) => {
  const ip = clientIp(req);
  if (isBlocked(ip)) {
    res.status(429).json({ error: 'too_many_attempts' });
    return;
  }
  const header = req.headers.authorization || '';
  const token = header.startsWith('Bearer ') ? header.slice(7).trim() : '';
  if (!tokenIsValid(token)) {
    recordFailure(ip);
    res.status(401).json({ error: 'invalid_token' });
    return;
  }
  res.json({
    version: pkg.version,
    uptimeSeconds: Math.floor((Date.now() - STARTED_AT) / 1000),
    desktop: (await checkVnc()) ? 'up' : 'down',
    wsPath: WS_PATH,
    node: process.version,
  });
});

app.use('/api', (_req, res) => {
  res.status(404).json({ error: 'not_found' });
});

app.use((err, _req, res, _next) => {
  if (err && err.type === 'entity.parse.failed') {
    res.status(400).json({ error: 'invalid_json' });
    return;
  }
  if (err && err.type === 'entity.too.large') {
    res.status(413).json({ error: 'payload_too_large' });
    return;
  }
  console.error('[gateway] unhandled error:', err);
  res.status(500).json({ error: 'internal_error' });
});

// ---------------------------------------------------------------------------
// Servidor HTTP + proxy WebSocket
// ---------------------------------------------------------------------------
const server = http.createServer(app);
server.keepAliveTimeout = 65 * 1000;
server.headersTimeout = 66 * 1000;

const proxy = httpProxy.createProxyServer({
  target: { host: VNC_HOST, port: VNC_PORT },
  ws: true,
  xfwd: true,
});

proxy.on('error', (err, _req, target) => {
  console.error('[gateway] proxy error:', err.message);
  if (target && typeof target.destroy === 'function') target.destroy();
});

function rejectUpgrade(socket, statusCode, message) {
  if (socket.destroyed) return;
  socket.write(
    `HTTP/1.1 ${statusCode} ${message}\r\n` +
      'Connection: close\r\n' +
      'Content-Length: 0\r\n\r\n'
  );
  socket.destroy();
}

server.on('upgrade', (req, socket, head) => {
  let url;
  try {
    url = new URL(req.url, 'http://gateway.local');
  } catch {
    rejectUpgrade(socket, 400, 'Bad Request');
    return;
  }

  if (url.pathname !== WS_PATH) {
    rejectUpgrade(socket, 404, 'Not Found');
    return;
  }

  if (!originOk(req)) {
    rejectUpgrade(socket, 403, 'Forbidden');
    return;
  }

  const ip = clientIp(req);
  if (isBlocked(ip)) {
    rejectUpgrade(socket, 429, 'Too Many Requests');
    return;
  }

  const token = url.searchParams.get('token') || '';
  if (!tokenIsValid(token)) {
    recordFailure(ip);
    rejectUpgrade(socket, 401, 'Unauthorized');
    return;
  }

  // Remove o token da URL antes de encaminhar ao KasmVNC e reescreve o caminho para "/".
  req.url = '/';
  socket.setKeepAlive(true, 30 * 1000);
  socket.on('error', () => socket.destroy());
  proxy.ws(req, socket, head, { target: { host: VNC_HOST, port: VNC_PORT } });
});

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------
server.listen(PORT, HOST, () => {
  console.log(`[gateway] v${pkg.version} ouvindo em http://${HOST}:${PORT}`);
  console.log(`[gateway] WebSocket: ws(s)://<host>${WS_PATH}?token=...  ->  ${VNC_HOST}:${VNC_PORT}`);
});

function shutdown(signal) {
  console.log(`[gateway] ${signal} recebido, encerrando...`);
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 5000).unref();
}

process.on('SIGTERM', () => shutdown('SIGTERM'));
process.on('SIGINT', () => shutdown('SIGINT'));
