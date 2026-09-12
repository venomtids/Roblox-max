#include "cortex/web_server.hpp"

namespace cortex {

std::string_view web_chat_html() noexcept {
    // Uma única resposta entrega toda a aplicação; não há CDN, npm ou arquivos externos.
    static constexpr std::string_view html = R"CORTEXHTML(<!doctype html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="dark">
  <meta name="theme-color" content="#090b12">
  <title>Cortex — IA Nativa</title>
  <style>
    :root {
      --bg: #090b12;
      --panel: #0f131d;
      --panel-2: #151a27;
      --panel-3: #1a2030;
      --line: rgba(255, 255, 255, .085);
      --line-strong: rgba(255, 255, 255, .14);
      --text: #f4f6fb;
      --muted: #929bad;
      --muted-2: #6f788a;
      --violet: #8b7cff;
      --violet-2: #6555e8;
      --cyan: #53d4dc;
      --green: #47d795;
      --danger: #ff7188;
      --shadow: 0 20px 60px rgba(0, 0, 0, .34);
      --radius: 18px;
    }

    * { box-sizing: border-box; }

    html, body { height: 100%; }

    body {
      margin: 0;
      overflow: hidden;
      color: var(--text);
      background:
        radial-gradient(circle at 72% -20%, rgba(104, 83, 232, .15), transparent 34%),
        radial-gradient(circle at -10% 90%, rgba(28, 173, 181, .08), transparent 30%),
        var(--bg);
      font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont,
                   "Segoe UI", sans-serif;
      -webkit-font-smoothing: antialiased;
    }

    button, textarea, select, input { font: inherit; }
    button { color: inherit; }

    .app {
      display: grid;
      grid-template-columns: 286px minmax(0, 1fr);
      width: 100%;
      height: 100%;
    }

    .sidebar {
      position: relative;
      z-index: 20;
      display: flex;
      flex-direction: column;
      min-width: 0;
      padding: 22px 16px 16px;
      border-right: 1px solid var(--line);
      background: rgba(12, 15, 23, .87);
      backdrop-filter: blur(22px);
    }

    .brand {
      display: flex;
      align-items: center;
      gap: 11px;
      padding: 0 8px 21px;
    }

    .logo, .avatar {
      display: grid;
      place-items: center;
      flex: 0 0 auto;
      color: white;
      font-weight: 800;
      background: linear-gradient(145deg, var(--violet), #4f46c8 58%, #3ab4bf);
      box-shadow: 0 8px 24px rgba(101, 85, 232, .3), inset 0 1px rgba(255,255,255,.25);
    }

    .logo { width: 37px; height: 37px; border-radius: 12px; font-size: 17px; }
    .brand-title { font-size: 16px; font-weight: 720; letter-spacing: -.02em; }
    .brand-subtitle { margin-top: 2px; color: var(--muted-2); font-size: 10px; letter-spacing: .1em; text-transform: uppercase; }

    .new-chat {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      width: 100%;
      min-height: 43px;
      padding: 0 14px;
      border: 1px solid rgba(139, 124, 255, .4);
      border-radius: 12px;
      color: #f7f5ff;
      background: linear-gradient(180deg, rgba(139,124,255,.18), rgba(101,85,232,.11));
      cursor: pointer;
      transition: border-color .2s, transform .2s, background .2s;
    }

    .new-chat:hover { border-color: rgba(139,124,255,.7); background: rgba(139,124,255,.2); transform: translateY(-1px); }
    .new-chat svg { width: 17px; }

    .self-edit-button {
      display: flex; align-items: center; justify-content: center; gap: 8px;
      width: 100%; min-height: 38px; margin-top: 12px; border: 1px solid var(--line);
      border-radius: 10px; color: #b9c2d2; background: rgba(255,255,255,.02);
      font-size: 11px; cursor: pointer;
    }
    .self-edit-button:hover { border-color: rgba(83,212,220,.35); color: #b9f1f4; }
    .self-edit-button svg { width: 15px; }
    .self-edit-button[hidden] { display: none; }

    .section-label {
      margin: 24px 9px 10px;
      color: var(--muted-2);
      font-size: 10px;
      font-weight: 700;
      letter-spacing: .11em;
      text-transform: uppercase;
    }

    .conversation-item {
      display: flex;
      align-items: center;
      gap: 10px;
      min-height: 39px;
      padding: 0 11px;
      border: 1px solid var(--line);
      border-radius: 10px;
      color: #c9cfdb;
      background: rgba(255,255,255,.025);
      font-size: 13px;
    }

    .conversation-item svg { width: 15px; color: var(--violet); }
    .conversation-title { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

    .settings {
      margin-top: 14px;
      padding: 4px 11px 12px;
      border: 1px solid var(--line);
      border-radius: 12px;
      background: rgba(255,255,255,.018);
    }

    .settings summary {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 10px 0 7px;
      color: #cbd1dc;
      font-size: 12px;
      cursor: pointer;
      list-style: none;
    }

    .settings summary::-webkit-details-marker { display: none; }
    .chevron { width: 14px; transition: transform .2s; }
    .settings[open] .chevron { transform: rotate(180deg); }

    .control { margin-top: 10px; }
    .control-line { display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px; color: var(--muted); font-size: 11px; }
    .control-value { color: #d9d4ff; font-variant-numeric: tabular-nums; }

    input[type="range"] {
      width: 100%; height: 4px; border-radius: 4px; outline: 0;
      accent-color: var(--violet); cursor: pointer;
    }

    select {
      width: 100%; height: 32px; padding: 0 8px;
      border: 1px solid var(--line); border-radius: 8px;
      color: #d8dce5; background: var(--panel-2); outline: none;
      font-size: 11px;
    }

    .toggle-line { display: flex; align-items: center; justify-content: space-between; margin-top: 13px; color: var(--muted); font-size: 11px; }
    .switch { position: relative; width: 34px; height: 19px; }
    .switch input { position: absolute; opacity: 0; }
    .switch span { position: absolute; inset: 0; border-radius: 12px; background: #303747; cursor: pointer; transition: .2s; }
    .switch span::after { content: ""; position: absolute; width: 13px; height: 13px; left: 3px; top: 3px; border-radius: 50%; background: #b9c0ce; transition: .2s; }
    .switch input:checked + span { background: var(--violet-2); }
    .switch input:checked + span::after { transform: translateX(15px); background: white; }

    .sidebar-spacer { flex: 1; min-height: 12px; }

    .runtime-card {
      padding: 12px;
      border: 1px solid var(--line);
      border-radius: 12px;
      background: linear-gradient(145deg, rgba(83,212,220,.045), rgba(139,124,255,.04));
    }

    .runtime-head { display: flex; align-items: center; gap: 8px; color: #d7dce6; font-size: 11px; font-weight: 650; }
    .status-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--muted-2); box-shadow: 0 0 0 3px rgba(111,120,138,.1); }
    .status-dot.online { background: var(--green); box-shadow: 0 0 0 3px rgba(71,215,149,.11), 0 0 12px rgba(71,215,149,.5); }
    .runtime-detail { margin-top: 8px; color: var(--muted-2); font-size: 10px; line-height: 1.55; }

    .main { display: flex; flex-direction: column; min-width: 0; height: 100%; }

    .topbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex: 0 0 65px;
      padding: 0 25px;
      border-bottom: 1px solid var(--line);
      background: rgba(9,11,18,.62);
      backdrop-filter: blur(14px);
    }

    .topbar-title { display: flex; align-items: center; gap: 10px; min-width: 0; }
    .topbar-title strong { font-size: 14px; letter-spacing: -.01em; }
    .native-pill { padding: 4px 8px; border: 1px solid rgba(83,212,220,.18); border-radius: 999px; color: #8ee6eb; background: rgba(83,212,220,.055); font-size: 9px; font-weight: 700; letter-spacing: .08em; }

    .model-info { display: flex; align-items: center; gap: 8px; color: var(--muted); font-size: 11px; }
    .model-info code { padding: 5px 8px; border: 1px solid var(--line); border-radius: 7px; color: #c5cbd7; background: rgba(255,255,255,.03); font-family: ui-monospace, SFMono-Regular, Consolas, monospace; }

    .mobile-menu { display: none; width: 37px; height: 37px; padding: 9px; border: 1px solid var(--line); border-radius: 10px; background: transparent; cursor: pointer; }

    .messages-wrap { position: relative; flex: 1; min-height: 0; overflow-y: auto; scrollbar-width: thin; scrollbar-color: #313849 transparent; }
    .messages { width: min(820px, calc(100% - 38px)); min-height: 100%; margin: 0 auto; padding: 34px 0 28px; }

    .welcome { display: grid; place-items: center; min-height: calc(100vh - 270px); padding: 34px 0; text-align: center; }
    .welcome-inner { width: min(620px, 100%); }
    .welcome-logo { width: 58px; height: 58px; margin: 0 auto 19px; border-radius: 18px; font-size: 24px; }
    .welcome h1 { margin: 0; font-size: clamp(25px, 4vw, 35px); line-height: 1.16; letter-spacing: -.045em; }
    .welcome h1 span { color: transparent; background: linear-gradient(90deg, #b2a8ff, #62d8df); background-clip: text; -webkit-background-clip: text; }
    .welcome p { width: min(520px, 100%); margin: 13px auto 25px; color: var(--muted); font-size: 13px; line-height: 1.65; }

    .suggestions { display: grid; grid-template-columns: repeat(2, 1fr); gap: 9px; }
    .suggestion { min-height: 70px; padding: 13px 14px; border: 1px solid var(--line); border-radius: 13px; text-align: left; color: #d6dbe5; background: rgba(255,255,255,.025); cursor: pointer; transition: .2s; }
    .suggestion:hover { border-color: rgba(139,124,255,.38); background: rgba(139,124,255,.06); transform: translateY(-1px); }
    .suggestion strong { display: block; margin-bottom: 5px; font-size: 12px; }
    .suggestion span { color: var(--muted-2); font-size: 10px; line-height: 1.4; }

    .message { display: grid; grid-template-columns: 34px minmax(0, 1fr); gap: 13px; padding: 16px 0; animation: arrive .25s ease-out both; }
    @keyframes arrive { from { opacity: 0; transform: translateY(5px); } }
    .avatar { width: 32px; height: 32px; border-radius: 10px; font-size: 12px; }
    .message.user .avatar { color: #d4d9e5; background: #252c3b; box-shadow: inset 0 1px rgba(255,255,255,.08); }
    .message-head { display: flex; align-items: baseline; gap: 8px; min-height: 22px; }
    .message-name { font-size: 12px; font-weight: 700; }
    .message-time, .message-meta { color: var(--muted-2); font-size: 9px; }
    .message-text { color: #dce0e9; font-size: 13px; line-height: 1.72; white-space: pre-wrap; overflow-wrap: anywhere; }
    .message.user .message-text { color: #c8ceda; }
    .message-meta { margin-top: 7px; }

    .thinking { display: flex; align-items: center; gap: 4px; height: 27px; }
    .thinking i { width: 5px; height: 5px; border-radius: 50%; background: var(--violet); animation: pulse 1.1s infinite ease-in-out; }
    .thinking i:nth-child(2) { animation-delay: .14s; }
    .thinking i:nth-child(3) { animation-delay: .28s; }
    @keyframes pulse { 0%, 60%, 100% { opacity: .28; transform: translateY(0); } 30% { opacity: 1; transform: translateY(-3px); } }

    .composer-area { flex: 0 0 auto; padding: 0 24px 18px; background: linear-gradient(180deg, transparent, var(--bg) 28%); }
    .composer-shell { width: min(840px, 100%); margin: 0 auto; }
    .composer {
      display: grid;
      grid-template-columns: minmax(0, 1fr) 42px;
      align-items: end;
      gap: 8px;
      padding: 9px 9px 9px 15px;
      border: 1px solid var(--line-strong);
      border-radius: 17px;
      background: rgba(21,26,39,.95);
      box-shadow: var(--shadow), inset 0 1px rgba(255,255,255,.025);
      transition: border-color .2s, box-shadow .2s;
    }
    .composer:focus-within { border-color: rgba(139,124,255,.48); box-shadow: 0 18px 55px rgba(0,0,0,.34), 0 0 0 3px rgba(139,124,255,.05); }

    textarea { width: 100%; max-height: 150px; min-height: 30px; padding: 5px 0 3px; resize: none; overflow-y: auto; border: 0; outline: 0; color: var(--text); background: transparent; font-size: 13px; line-height: 1.55; }
    textarea::placeholder { color: #687184; }

    .send { display: grid; place-items: center; width: 39px; height: 39px; border: 0; border-radius: 11px; background: linear-gradient(145deg, var(--violet), var(--violet-2)); cursor: pointer; box-shadow: 0 7px 20px rgba(101,85,232,.25); transition: transform .2s, opacity .2s; }
    .send:hover:not(:disabled) { transform: translateY(-1px); }
    .send:disabled { opacity: .35; cursor: not-allowed; box-shadow: none; }
    .send svg { width: 17px; }
    .composer-note { margin-top: 9px; color: var(--muted-2); text-align: center; font-size: 9px; }

    .editor-overlay {
      position: fixed; z-index: 80; inset: 0; display: none; place-items: center;
      padding: 18px; background: rgba(3,5,10,.78); backdrop-filter: blur(8px);
    }
    .editor-overlay.open { display: grid; }
    .editor-modal {
      width: min(780px, 100%); max-height: min(800px, 92vh); overflow: auto;
      padding: 22px; border: 1px solid var(--line-strong); border-radius: 18px;
      background: #111621; box-shadow: var(--shadow);
    }
    .editor-head { display: flex; justify-content: space-between; gap: 14px; align-items: start; }
    .editor-head h2 { margin: 0; font-size: 17px; }
    .editor-head p { margin: 7px 0 0; color: var(--muted); font-size: 11px; line-height: 1.55; }
    .close-editor { width: 32px; height: 32px; border: 1px solid var(--line); border-radius: 9px; background: transparent; cursor: pointer; }
    .editor-field { display: block; margin-top: 16px; color: var(--muted); font-size: 11px; }
    .editor-field input, .editor-field textarea {
      display: block; width: 100%; margin-top: 7px; padding: 10px 11px;
      border: 1px solid var(--line); border-radius: 9px; outline: 0;
      color: var(--text); background: #0b0f17; font-size: 12px;
    }
    .editor-field textarea { min-height: 82px; max-height: 150px; resize: vertical; }
    .editor-field input:focus, .editor-field textarea:focus { border-color: rgba(139,124,255,.5); }
    .editor-actions { display: flex; justify-content: flex-end; gap: 9px; margin-top: 15px; }
    .editor-action { min-height: 37px; padding: 0 14px; border: 1px solid var(--line); border-radius: 9px; background: var(--panel-3); cursor: pointer; font-size: 11px; }
    .editor-action.primary { border-color: rgba(139,124,255,.4); background: var(--violet-2); color: white; }
    .editor-action:disabled { opacity: .4; cursor: wait; }
    .code-preview { display: none; max-height: 280px; overflow: auto; margin: 15px 0 0; padding: 12px; border: 1px solid var(--line); border-radius: 10px; color: #b8c2d4; background: #090d14; font: 10px/1.55 ui-monospace, SFMono-Regular, Consolas, monospace; white-space: pre; }
    .code-preview.visible { display: block; }
    .editor-warning { margin-top: 13px; color: #e6b875; font-size: 10px; line-height: 1.5; }

    .toast { position: fixed; z-index: 60; right: 20px; bottom: 20px; max-width: 330px; padding: 12px 15px; border: 1px solid rgba(255,113,136,.25); border-radius: 11px; color: #ffdce2; background: rgba(58,24,32,.96); box-shadow: var(--shadow); font-size: 11px; opacity: 0; transform: translateY(10px); pointer-events: none; transition: .25s; }
    .toast.show { opacity: 1; transform: translateY(0); }
    .backdrop { display: none; }

    @media (max-width: 760px) {
      .app { grid-template-columns: 1fr; }
      .sidebar { position: fixed; inset: 0 auto 0 0; width: min(286px, 86vw); transform: translateX(-102%); transition: transform .25s ease; box-shadow: 20px 0 60px rgba(0,0,0,.45); }
      .sidebar.open { transform: translateX(0); }
      .backdrop { position: fixed; z-index: 15; inset: 0; display: block; visibility: hidden; opacity: 0; background: rgba(0,0,0,.55); transition: .25s; }
      .backdrop.open { visibility: visible; opacity: 1; }
      .mobile-menu { display: grid; place-items: center; }
      .topbar { padding: 0 14px; }
      .topbar-title { gap: 8px; }
      .native-pill, .model-info span { display: none; }
      .messages { width: calc(100% - 28px); padding-top: 20px; }
      .suggestions { grid-template-columns: 1fr; }
      .suggestion:nth-child(n+4) { display: none; }
      .composer-area { padding: 0 12px 12px; }
      .welcome { min-height: calc(100vh - 235px); }
      .message { grid-template-columns: 30px minmax(0,1fr); gap: 10px; }
      .avatar { width: 29px; height: 29px; border-radius: 9px; }
    }

    @media (prefers-reduced-motion: reduce) {
      *, *::before, *::after { animation-duration: .01ms !important; transition-duration: .01ms !important; }
    }
  </style>
</head>
<body>
  <div class="app">
    <aside class="sidebar" id="sidebar" aria-label="Navegação e configurações">
      <div class="brand">
        <div class="logo">C</div>
        <div>
          <div class="brand-title">Cortex</div>
          <div class="brand-subtitle">Native Intelligence</div>
        </div>
      </div>

      <button class="new-chat" id="newChat" type="button">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M12 5v14M5 12h14"/></svg>
        Nova conversa
      </button>

      <div class="section-label">Conversa atual</div>
      <div class="conversation-item">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M21 15a4 4 0 0 1-4 4H8l-5 3V7a4 4 0 0 1 4-4h10a4 4 0 0 1 4 4z"/></svg>
        <span class="conversation-title" id="conversationTitle">Nova conversa</span>
      </div>

      <div class="section-label">Inferência</div>
      <details class="settings" open>
        <summary>
          Ajustes da resposta
          <svg class="chevron" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="m6 9 6 6 6-6"/></svg>
        </summary>
        <div class="control">
          <div class="control-line"><span>Modo de potência</span></div>
          <select id="powerMode" aria-label="Modo de potência">
            <option value="flash">Flash · resposta rápida</option>
            <option value="medium" selected>Medium · equilibrado</option>
            <option value="high">Alta potência · revisão dupla</option>
          </select>
        </div>
        <div class="control">
          <div class="control-line"><span>Temperatura</span><span class="control-value" id="temperatureValue">0.5</span></div>
          <input id="temperature" type="range" min="0.2" max="1.5" value="0.5" step="0.1" aria-label="Temperatura">
        </div>
        <div class="control">
          <div class="control-line"><span>Top-k</span></div>
          <select id="topK" aria-label="Top-k"><option>4</option><option>8</option><option selected>20</option><option>24</option><option>40</option><option>48</option></select>
        </div>
        <div class="control">
          <div class="control-line"><span>Comprimento máximo</span></div>
          <select id="maxTokens" aria-label="Máximo de tokens"><option>64</option><option>96</option><option>128</option><option>192</option><option>256</option><option>512</option><option selected>1024</option><option>2048</option><option>4096</option></select>
        </div>
        <div class="toggle-line">
          <span>Modelo GGUF local</span>
          <label class="switch"><input id="languageApi" type="checkbox" checked><span></span></label>
        </div>
        <div class="toggle-line">
          <span>Pesquisar quando necessário</span>
          <label class="switch"><input id="internet" type="checkbox"><span></span></label>
        </div>
        <div class="toggle-line">
          <span>Memória automática</span>
          <label class="switch"><input id="remember" type="checkbox"><span></span></label>
        </div>
      </details>

      <button class="self-edit-button" id="openKnowledge" type="button">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M4 5.5A2.5 2.5 0 0 1 6.5 3H11v16H6.5A2.5 2.5 0 0 0 4 21.5Z"/><path d="M20 5.5A2.5 2.5 0 0 0 17.5 3H13v16h4.5a2.5 2.5 0 0 1 2.5 2.5Z"/></svg>
        Ensinar documento
      </button>

      <button class="self-edit-button" id="openSelfEditor" type="button" hidden>
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="m14.7 6.3 3 3M5 19l3.8-.8L19 7a2.1 2.1 0 0 0-3-3L4.8 14.2 4 18Z"/><path d="M12 20h8"/></svg>
        Autoevolução controlada
      </button>

      <div class="sidebar-spacer"></div>
      <div class="runtime-card">
        <div class="runtime-head"><i class="status-dot" id="statusDot"></i><span id="runtimeStatus">Conectando ao núcleo…</span></div>
        <div class="runtime-detail" id="runtimeDetail">Servidor C++20 · sem Python ou Node.js</div>
      </div>
    </aside>

    <div class="backdrop" id="backdrop"></div>

    <main class="main">
      <header class="topbar">
        <div class="topbar-title">
          <button class="mobile-menu" id="mobileMenu" type="button" aria-label="Abrir menu">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M4 7h16M4 12h16M4 17h16"/></svg>
          </button>
          <strong>Cortex Cognitive Engine</strong>
          <span class="native-pill">C++20 NATIVO</span>
        </div>
        <div class="model-info"><span>Modelo</span><code id="modelLabel">carregando</code></div>
      </header>

      <section class="messages-wrap" id="scrollArea">
        <div class="messages" id="messages" aria-live="polite"></div>
      </section>

      <div class="composer-area">
        <div class="composer-shell">
          <form class="composer" id="composer">
            <textarea id="prompt" rows="1" maxlength="16000" placeholder="Converse com Cortex…" aria-label="Mensagem para Cortex"></textarea>
            <button class="send" id="send" type="submit" disabled aria-label="Enviar mensagem">
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="m5 12 14-7-4 14-3-6-7-1Z"/><path d="m12 13 7-8"/></svg>
            </button>
          </form>
          <div class="composer-note">Enter envia · Shift+Enter quebra linha · qualidade depende do modelo ativo</div>
        </div>
      </div>
    </main>
  </div>

  <div class="editor-overlay" id="knowledgeOverlay" role="dialog" aria-modal="true" aria-labelledby="knowledgeTitle">
    <div class="editor-modal">
      <div class="editor-head">
        <div><h2 id="knowledgeTitle">Aprendizado documental persistente</h2><p>Cole material próprio ou legalmente utilizável. A Cortex fragmenta, cria índices híbridos e recupera o conteúdo offline nas próximas conversas.</p></div>
        <button class="close-editor" id="closeKnowledge" type="button" aria-label="Fechar">×</button>
      </div>
      <label class="editor-field">Nome ou origem do documento
        <input id="knowledgeSource" maxlength="512" placeholder="Ex.: manual do projeto, capítulo 3">
      </label>
      <label class="editor-field">Conteúdo
        <textarea id="knowledgeText" maxlength="48000" style="min-height:220px" placeholder="Cole aqui até 48 KB de texto ou código…"></textarea>
      </label>
      <div class="editor-warning">O conteúdo será gravado na memória local. Ele não altera pesos neurais e não é enviado a uma API.</div>
      <div class="editor-actions"><button class="editor-action primary" id="ingestKnowledge" type="button">Indexar e aprender</button></div>
      <div class="editor-warning" id="knowledgeResult" hidden></div>
    </div>
  </div>

  <div class="editor-overlay" id="editorOverlay" role="dialog" aria-modal="true" aria-labelledby="editorTitle">
    <div class="editor-modal">
      <div class="editor-head">
        <div><h2 id="editorTitle">Autoevolução supervisionada</h2><p id="editorScopeText">Audite e gere uma transformação nativa ou pelo GGUF local. Revise e confirme: nenhuma mudança é aplicada silenciosamente.</p></div>
        <button class="close-editor" id="closeEditor" type="button" aria-label="Fechar">×</button>
      </div>
      <div class="editor-warning" id="scopeWarning" hidden></div>
      <label class="editor-field">Caminho do arquivo
        <input id="editFile" value="src/agent.cpp" maxlength="4096" spellcheck="false">
      </label>
      <label class="check-row" id="createFileRow" hidden>
        <input id="createFile" type="checkbox">
        <span><strong>Criar se não existir</strong><small>O caminho e o conteúdo completos serão mostrados antes da autorização.</small></span>
      </label>
      <label class="editor-field">Objetivo da mudança
        <textarea id="editInstruction" maxlength="1024" placeholder="Ex.: melhore o tratamento de perguntas curtas sem alterar as APIs públicas"></textarea>
      </label>
      <label class="editor-field">Comportamento observado (opcional)
        <textarea id="editBehavior" maxlength="1024" placeholder="Cole uma resposta problemática ou descreva uma regressão reproduzível"></textarea>
      </label>
      <div class="editor-actions">
        <button class="editor-action primary" id="autoEvolve" type="button">Sugerir evolução automática</button>
        <button class="editor-action" id="auditCode" type="button">Auditar fonte</button>
        <button class="editor-action" id="refineCode" type="button">Auditar + autorrefinar</button>
        <button class="editor-action" id="proposeCode" type="button">Proposta manual</button>
      </div>
      <div class="editor-warning" id="auditSummary" hidden></div>
      <pre class="code-preview" id="codePreview"></pre>
      <div id="applyArea" hidden>
        <div class="editor-warning" id="proposalRisk">Revise todo o conteúdo. A aplicação cria backup e exige recompilar/testar.</div>
        <label class="editor-field">Confirmação
          <input id="editConfirmation" autocomplete="off" spellcheck="false">
        </label>
        <div class="editor-actions"><button class="editor-action primary" id="applyCode" type="button">Aplicar alteração</button></div>
      </div>
      <div id="rollbackArea" hidden>
        <div class="editor-warning">Se o teste falhar, reverta enquanto o arquivo não tiver outras alterações.</div>
        <label class="editor-field">Confirmação de rollback
          <input id="rollbackConfirmation" autocomplete="off" spellcheck="false">
        </label>
        <div class="editor-actions"><button class="editor-action" id="rollbackCode" type="button">Restaurar backup</button></div>
      </div>
      <div id="validationArea" hidden>
        <label class="editor-field">Comando de validação
          <textarea id="validationCommand" maxlength="16384" spellcheck="false" placeholder="make test && make sanitize-test && make release"></textarea>
        </label>
        <div class="editor-actions"><button class="editor-action" id="stageValidation" type="button">Preparar validação</button></div>
        <div id="validationConsent" hidden>
          <div class="editor-warning" id="validationPreview"></div>
          <label class="editor-field">Permissão para executar
            <input id="validationConfirmation" autocomplete="off" spellcheck="false">
          </label>
          <div class="editor-actions"><button class="editor-action primary" id="runValidation" type="button">Executar comando autorizado</button></div>
        </div>
        <pre class="code-preview" id="validationOutput"></pre>
      </div>
    </div>
  </div>

  <div class="toast" id="toast" role="alert"></div>

  <script>
    "use strict";

    const byId = (id) => document.getElementById(id);
    const messages = byId("messages");
    const scrollArea = byId("scrollArea");
    const prompt = byId("prompt");
    const send = byId("send");
    const composer = byId("composer");
    const conversationTitle = byId("conversationTitle");
    const temperature = byId("temperature");
    const temperatureValue = byId("temperatureValue");
    const powerMode = byId("powerMode");
    const maxTokens = byId("maxTokens");
    const topK = byId("topK");
    const sidebar = byId("sidebar");
    const backdrop = byId("backdrop");
    let busy = false;
    let history = [];
    let toastTimer = 0;
    let currentProposal = 0;
    let currentApplied = 0;
    let currentValidation = 0;
    let maxTokensTouched = false;
    let topKTouched = false;
    let temperatureTouched = false;
    let powerTouched = false;

    const suggestions = [
      ["Aprender programação", "Explique RAII e ownership em programação"],
      ["Escrita longa", "Redija um artigo longo sobre inteligência artificial responsável"],
      ["Criar uma história", "Escreva uma história criativa sobre robôs aprendendo com erros"],
      ["Analisar um texto", "analise este texto: Clareza exige estrutura, evidência e revisão."],
      ["Pesquisar e memorizar", "pesquise na internet por computação quântica"],
      ["Autoentendimento", "Quais são suas capacidades e limitações?"]
    ];

    function welcome() {
      messages.replaceChildren();
      const root = document.createElement("div");
      root.className = "welcome";
      const inner = document.createElement("div");
      inner.className = "welcome-inner";
      inner.innerHTML = '<div class="logo welcome-logo">C</div><h1>Converse com sua <span>IA nativa</span></h1><p>O Cognitive Mesh oferece síntese, escrita, código, memória híbrida, documentos e pesquisa sem chave. <code>--x40</code> adiciona IBM Granite 4.2 3B local.</p>';
      const grid = document.createElement("div");
      grid.className = "suggestions";
      for (const [title, text] of suggestions) {
        const button = document.createElement("button");
        button.type = "button";
        button.className = "suggestion";
        const strong = document.createElement("strong");
        strong.textContent = title;
        const detail = document.createElement("span");
        detail.textContent = text;
        button.append(strong, detail);
        button.addEventListener("click", () => sendMessage(text));
        grid.appendChild(button);
      }
      inner.appendChild(grid);
      root.appendChild(inner);
      messages.appendChild(root);
    }

    function timeLabel() {
      return new Intl.DateTimeFormat("pt-BR", {hour: "2-digit", minute: "2-digit"}).format(new Date());
    }

    function appendMessage(role, text, meta = "", persist = true) {
      const welcomeNode = messages.querySelector(".welcome");
      if (welcomeNode) welcomeNode.remove();

      const article = document.createElement("article");
      article.className = `message ${role}`;
      const avatar = document.createElement("div");
      avatar.className = "avatar";
      avatar.textContent = role === "user" ? "V" : "C";

      const body = document.createElement("div");
      const head = document.createElement("div");
      head.className = "message-head";
      const name = document.createElement("span");
      name.className = "message-name";
      name.textContent = role === "user" ? "Você" : "Cortex";
      const clock = document.createElement("span");
      clock.className = "message-time";
      clock.textContent = timeLabel();
      head.append(name, clock);

      const content = document.createElement("div");
      content.className = "message-text";
      content.textContent = text;
      body.append(head, content);
      if (meta) {
        const metadata = document.createElement("div");
        metadata.className = "message-meta";
        metadata.textContent = meta;
        body.appendChild(metadata);
      }
      article.append(avatar, body);
      messages.appendChild(article);

      if (persist) {
        history.push({role, text, meta});
        history = history.slice(-40);
        localStorage.setItem("cortex-chat-history-v4", JSON.stringify(history));
      }
      requestAnimationFrame(() => { scrollArea.scrollTop = scrollArea.scrollHeight; });
      return article;
    }

    function appendThinking() {
      const article = appendMessage("assistant", "", "", false);
      const content = article.querySelector(".message-text");
      content.classList.add("thinking");
      content.innerHTML = "<i></i><i></i><i></i>";
      return article;
    }

    function showToast(message) {
      const toast = byId("toast");
      toast.textContent = message;
      toast.classList.add("show");
      clearTimeout(toastTimer);
      toastTimer = setTimeout(() => toast.classList.remove("show"), 4200);
    }

    function applyPowerPreset(markTouched = true) {
      const presets = {
        flash: {temperature: "0.3", topK: "8", maxTokens: "256"},
        medium: {temperature: "0.5", topK: "20", maxTokens: "1024"},
        high: {temperature: "0.6", topK: "40", maxTokens: "4096"}
      };
      const preset = presets[powerMode.value] || presets.medium;
      temperature.value = preset.temperature;
      temperatureValue.textContent = preset.temperature;
      topK.value = preset.topK;
      maxTokens.value = preset.maxTokens;
      if (markTouched) {
        powerTouched = true;
        temperatureTouched = true;
        topKTouched = true;
        maxTokensTouched = true;
      }
    }

    function resizePrompt() {
      prompt.style.height = "auto";
      prompt.style.height = `${Math.min(prompt.scrollHeight, 150)}px`;
      send.disabled = busy || !prompt.value.trim();
    }

    function setBusy(value) {
      busy = value;
      prompt.disabled = value;
      send.disabled = value || !prompt.value.trim();
    }

    async function refreshStatus() {
      try {
        const response = await fetch("/api/status", {cache: "no-store"});
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || "núcleo indisponível");
        byId("statusDot").classList.add("online");
        const geminiOnline = data.language_api && data.language_provider === "gemini";
        const openModelOnline = data.language_api && data.language_provider === "local-open-model";
        const apiWaiting = data.language_api_state === "missing-key";
        byId("runtimeStatus").textContent = openModelOnline ? "IA aberta local conectada"
          : geminiOnline ? "Provedor opcional conectado"
          : data.language_api ? "Modelo linguístico conectado"
          : apiWaiting ? "Provedor sem chave · modo local" : "Cortex local confiável";
        const activeParameters = Number(data.language_parameters || 0);
        const contextDetail = Number(data.language_context || 0)
          ? ` · ${Number(data.language_context).toLocaleString("pt-BR")} ctx` : "";
        const nativeDetail = `${data.knowledge_topics || 0} temas curados · ${data.memories} memórias`;
        byId("runtimeDetail").textContent = activeParameters
          ? `${activeParameters.toLocaleString("pt-BR")} parâmetros do modelo${contextDetail} · ${nativeDetail}`
          : data.language_api
            ? `Parâmetros não informados${contextDetail} · ${nativeDetail}`
            : `${data.parameters.toLocaleString("pt-BR")} parâmetros GRU + Cognitive Mesh · ${nativeDetail}`;
        byId("modelLabel").textContent = data.language_api ? data.language_model
          : apiWaiting ? `${data.language_model} · modo local` : data.language_model;
        if (!powerTouched && data.power_mode) {
          powerMode.value = data.power_mode;
          applyPowerPreset(false);
        }
        byId("languageApi").disabled = !data.language_api;
        if (!data.language_api) byId("languageApi").checked = false;
        byId("internet").disabled = !data.internet;
        byId("openSelfEditor").hidden = !data.self_edit;
        const unrestricted = Boolean(data.unrestricted_self_edit);
        byId("createFileRow").hidden = !unrestricted;
        byId("validationArea").hidden = !unrestricted;
        byId("scopeWarning").hidden = !unrestricted;
        byId("scopeWarning").textContent = unrestricted
          ? "Acesso irrestrito supervisionado: caminhos absolutos e arquivos novos são aceitos. Cada escrita ainda exige sua autorização e as permissões da conta/Windows."
          : "";
        byId("editorScopeText").textContent = unrestricted
          ? "A Cortex pode substituir ou criar arquivos em qualquer caminho autorizado pelo sistema. Revise o conteúdo e conceda permissão por operação."
          : "Audite e gere uma proposta nativa determinística ou pelo GGUF local. Revise e confirme: nenhuma mudança é aplicada silenciosamente.";
        byId("autoEvolve").disabled = !data.self_programming;
        byId("refineCode").disabled = !data.self_programming;
        byId("proposeCode").disabled = !data.self_programming;
      } catch (error) {
        byId("statusDot").classList.remove("online");
        byId("runtimeStatus").textContent = "Núcleo offline";
        byId("runtimeDetail").textContent = String(error.message || error);
      }
    }

    async function sendMessage(forcedText = "") {
      if (busy) return;
      const text = (forcedText || prompt.value).trim();
      if (!text) return;
      if (history.length === 0) conversationTitle.textContent = text.slice(0, 38);
      // As últimas mensagens dão continuidade explícita; memórias cuidam do longo prazo.
      const conversation = history.slice(-10).map((item) =>
        `${item.role === "user" ? "Usuário" : "Assistente"}: ${item.text}`
      ).join("\n").slice(-12000);

      appendMessage("user", text);
      prompt.value = "";
      resizePrompt();
      setBusy(true);
      const thinking = appendThinking();

      try {
        const response = await fetch("/api/chat", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({
            message: text,
            conversation,
            power_mode: powerMode.value,
            temperature: Number(temperature.value),
            top_k: Number(byId("topK").value),
            max_tokens: Number(byId("maxTokens").value),
            language_api: byId("languageApi").checked,
            internet: byId("internet").checked,
            remember: byId("remember").checked
          })
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
        thinking.remove();
        const recalled = Array.isArray(data.recalled) ? data.recalled.length : 0;
        const provider = data.provider === "local-open-model" ? `GGUF local · ${data.provider_model}`
          : data.provider === "language-api" ? `Provedor opcional · ${data.provider_model}`
          : data.provider === "internet" ? `Internet · ${data.internet_source_name || data.provider_model || "fonte HTTPS"}`
          : String(data.provider || "").startsWith("local-") ? `Local · ${data.provider_model}`
          : "Cortex local";
        const learned = data.learned_from_web ? " · salvo para uso offline" : "";
        const quality = Number.isFinite(Number(data.quality_score))
          ? ` · qualidade ${Math.round(Number(data.quality_score) * 100)}%` : "";
        const revised = data.response_revised ? " · revisão dupla" : "";
        const mode = data.power_mode ? ` · ${data.power_mode}` : "";
        const meta = `${provider}${mode} · ${data.latency_ms} ms${quality}${revised}${recalled ? ` · ${recalled} memória${recalled > 1 ? "s" : ""}` : ""}${learned}`;
        appendMessage("assistant", data.reply || "O modelo encerrou a geração sem produzir texto.", meta);
        if (data.warning) showToast(data.warning);
        refreshStatus();
      } catch (error) {
        thinking.remove();
        appendMessage("assistant", "Não consegui concluir essa inferência.", "falha de comunicação", false);
        showToast(String(error.message || error));
      } finally {
        setBusy(false);
        prompt.focus();
      }
    }

    function openKnowledgeEditor() {
      byId("knowledgeOverlay").classList.add("open");
      byId("knowledgeSource").focus();
    }

    function closeKnowledgeEditor() {
      byId("knowledgeOverlay").classList.remove("open");
    }

    async function ingestKnowledgeDocument() {
      const source = byId("knowledgeSource").value.trim();
      const text = byId("knowledgeText").value.trim();
      if (!source || !text) return showToast("Informe a origem e o conteúdo do documento.");
      const button = byId("ingestKnowledge");
      button.disabled = true;
      button.textContent = "Indexando…";
      try {
        const response = await fetch("/api/knowledge/ingest", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({source, text, chunk_bytes: 2048})
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
        byId("knowledgeResult").textContent = `${data.chunks} fragmento(s) processado(s), ${data.new_chunks ?? data.chunks} novo(s); ${data.memories} memórias persistentes no total.`;
        byId("knowledgeResult").hidden = false;
        byId("knowledgeText").value = "";
        showToast("Documento aprendido e disponível para recuperação offline.");
        refreshStatus();
      } catch (error) {
        showToast(String(error.message || error));
      } finally {
        button.disabled = false;
        button.textContent = "Indexar e aprender";
      }
    }

    function openCodeEditor() {
      byId("editorOverlay").classList.add("open");
      byId("editInstruction").focus();
    }

    function closeCodeEditor() {
      byId("editorOverlay").classList.remove("open");
    }

    function showCodeProposal(data) {
      currentProposal = data.proposal_id;
      byId("codePreview").textContent = data.replacement;
      byId("codePreview").classList.add("visible");
      byId("editConfirmation").placeholder = data.confirmation;
      byId("editConfirmation").value = "";
      const operation = data.creates_file ? "CRIAR ARQUIVO" : "SUBSTITUIR ARQUIVO";
      byId("proposalRisk").textContent = `${operation} · risco ${data.risk_level || "não informado"} · ${data.audit_summary || "revise a alteração completa"}. Nada será escrito sem a confirmação abaixo.`;
      byId("applyArea").hidden = false;
    }

    async function suggestAutomaticEvolution() {
      const button = byId("autoEvolve");
      button.disabled = true;
      button.textContent = "Auditando comportamento e fontes…";
      try {
        const response = await fetch("/api/code/evolve", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({focus: byId("editInstruction").value.trim()})
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
        byId("editFile").value = data.file;
        byId("auditSummary").textContent = `Seleção automática: ${data.selection_reason}. ${data.audit_summary || "Revise o patch proposto."}`;
        byId("auditSummary").hidden = false;
        showCodeProposal(data);
      } catch (error) {
        showToast(String(error.message || error));
      } finally {
        button.disabled = false;
        button.textContent = "Sugerir evolução automática";
      }
    }

    async function auditCodeSource() {
      const file = byId("editFile").value.trim();
      if (!file) return showToast("Informe o arquivo que será auditado.");
      const button = byId("auditCode");
      button.disabled = true;
      try {
        const response = await fetch("/api/code/audit", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({file, behavior: byId("editBehavior").value.trim()})
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
        const findings = data.findings && data.findings.length ? data.findings.join("; ") : "nenhum alerta estático";
        byId("auditSummary").textContent = `Manutenibilidade ${Math.round(data.maintainability_score)}/100 · ${data.lines} linhas · ${findings}`;
        byId("auditSummary").hidden = false;
        if (!byId("editInstruction").value.trim()) byId("editInstruction").value = data.suggested_instruction;
      } catch (error) {
        showToast(String(error.message || error));
      } finally {
        button.disabled = false;
      }
    }

    async function refineCodeChange() {
      const file = byId("editFile").value.trim();
      const objective = byId("editInstruction").value.trim();
      if (!file || !objective) return showToast("Informe o arquivo e o objetivo da evolução.");
      const button = byId("refineCode");
      button.disabled = true;
      button.textContent = "Analisando…";
      try {
        const response = await fetch("/api/code/refine", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({file, objective, behavior: byId("editBehavior").value.trim()})
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
        showCodeProposal(data);
      } catch (error) {
        showToast(String(error.message || error));
      } finally {
        button.disabled = false;
        button.textContent = "Auditar + autorrefinar";
      }
    }

    async function proposeCodeChange() {
      const file = byId("editFile").value.trim();
      const instruction = byId("editInstruction").value.trim();
      if (!file || !instruction) return showToast("Informe o arquivo e a mudança desejada.");
      const button = byId("proposeCode");
      button.disabled = true;
      button.textContent = "Gerando…";
      try {
        const response = await fetch("/api/code/propose", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({file, instruction, create: byId("createFile").checked})
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
        showCodeProposal(data);
      } catch (error) {
        showToast(String(error.message || error));
      } finally {
        button.disabled = false;
        button.textContent = "Proposta manual";
      }
    }

    async function applyCodeChange() {
      if (!currentProposal) return;
      const button = byId("applyCode");
      button.disabled = true;
      try {
        const response = await fetch("/api/code/apply", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({
            proposal_id: currentProposal,
            confirmation: byId("editConfirmation").value.trim()
          })
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
        showToast(data.created_file
          ? "Arquivo criado com autorização. Valide o sistema; o rollback poderá removê-lo."
          : "Arquivo alterado com backup. Compile e teste; reverta aqui se houver regressão.");
        currentApplied = data.proposal_id;
        byId("rollbackConfirmation").placeholder = data.rollback_confirmation;
        byId("rollbackConfirmation").value = "";
        byId("rollbackArea").hidden = false;
        currentProposal = 0;
        byId("applyArea").hidden = true;
      } catch (error) {
        showToast(String(error.message || error));
      } finally {
        button.disabled = false;
      }
    }

    async function rollbackCodeChange() {
      if (!currentApplied) return;
      const button = byId("rollbackCode");
      button.disabled = true;
      try {
        const response = await fetch("/api/code/rollback", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({
            proposal_id: currentApplied,
            confirmation: byId("rollbackConfirmation").value.trim()
          })
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
        currentApplied = 0;
        byId("rollbackArea").hidden = true;
        showToast("Backup restaurado. Recompile e reinicie para voltar à versão anterior.");
      } catch (error) {
        showToast(String(error.message || error));
      } finally {
        button.disabled = false;
      }
    }

    async function stageValidationCommand() {
      const command = byId("validationCommand").value.trim();
      if (!command) return showToast("Informe o comando exato que deseja validar.");
      const button = byId("stageValidation");
      button.disabled = true;
      try {
        const response = await fetch("/api/validation/stage", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({command})
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
        currentValidation = data.validation_id;
        byId("validationPreview").textContent = `Comando que será executado com as permissões da Cortex:\n${data.command}`;
        byId("validationConfirmation").placeholder = data.confirmation;
        byId("validationConfirmation").value = "";
        byId("validationConsent").hidden = false;
      } catch (error) {
        showToast(String(error.message || error));
      } finally {
        button.disabled = false;
      }
    }

    async function runValidationCommand() {
      if (!currentValidation) return;
      const button = byId("runValidation");
      button.disabled = true;
      button.textContent = "Executando…";
      try {
        const response = await fetch("/api/validation/run", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({
            validation_id: currentValidation,
            confirmation: byId("validationConfirmation").value.trim()
          })
        });
        const data = await response.json();
        if (!response.ok || !data.ok) throw new Error(data.error || `HTTP ${response.status}`);
        byId("validationOutput").textContent = `Exit code: ${data.exit_code}\nLog: ${data.full_log}\n\n${data.output || "(sem saída)"}`;
        byId("validationOutput").classList.add("visible");
        showToast(data.exit_code === 0 ? "Validação concluída com sucesso." : `Validação terminou com código ${data.exit_code}.`);
        currentValidation = 0;
        byId("validationConsent").hidden = true;
      } catch (error) {
        showToast(String(error.message || error));
      } finally {
        button.disabled = false;
        button.textContent = "Executar comando autorizado";
      }
    }

    function clearConversation() {
      if (busy) return;
      history = [];
      localStorage.removeItem("cortex-chat-history-v4");
      conversationTitle.textContent = "Nova conversa";
      welcome();
      prompt.focus();
      closeSidebar();
    }

    function openSidebar() { sidebar.classList.add("open"); backdrop.classList.add("open"); }
    function closeSidebar() { sidebar.classList.remove("open"); backdrop.classList.remove("open"); }

    composer.addEventListener("submit", (event) => { event.preventDefault(); sendMessage(); });
    prompt.addEventListener("input", resizePrompt);
    prompt.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        sendMessage();
      }
    });
    temperature.addEventListener("input", () => {
      temperatureTouched = true;
      temperatureValue.textContent = Number(temperature.value).toFixed(1);
    });
    powerMode.addEventListener("change", () => applyPowerPreset(true));
    topK.addEventListener("change", () => { topKTouched = true; });
    maxTokens.addEventListener("change", () => { maxTokensTouched = true; });
    byId("newChat").addEventListener("click", clearConversation);
    byId("mobileMenu").addEventListener("click", openSidebar);
    backdrop.addEventListener("click", closeSidebar);
    byId("openKnowledge").addEventListener("click", openKnowledgeEditor);
    byId("closeKnowledge").addEventListener("click", closeKnowledgeEditor);
    byId("ingestKnowledge").addEventListener("click", ingestKnowledgeDocument);
    byId("openSelfEditor").addEventListener("click", openCodeEditor);
    byId("closeEditor").addEventListener("click", closeCodeEditor);
    byId("autoEvolve").addEventListener("click", suggestAutomaticEvolution);
    byId("auditCode").addEventListener("click", auditCodeSource);
    byId("refineCode").addEventListener("click", refineCodeChange);
    byId("proposeCode").addEventListener("click", proposeCodeChange);
    byId("applyCode").addEventListener("click", applyCodeChange);
    byId("rollbackCode").addEventListener("click", rollbackCodeChange);
    byId("stageValidation").addEventListener("click", stageValidationCommand);
    byId("runValidation").addEventListener("click", runValidationCommand);
    byId("editorOverlay").addEventListener("click", (event) => {
      if (event.target === byId("editorOverlay")) closeCodeEditor();
    });
    byId("knowledgeOverlay").addEventListener("click", (event) => {
      if (event.target === byId("knowledgeOverlay")) closeKnowledgeEditor();
    });

    try {
      const saved = JSON.parse(localStorage.getItem("cortex-chat-history-v4") || "[]");
      if (Array.isArray(saved) && saved.length) {
        history = [];
        messages.replaceChildren();
        for (const item of saved.slice(-40)) {
          if ((item.role === "user" || item.role === "assistant") && typeof item.text === "string") {
            appendMessage(item.role, item.text, typeof item.meta === "string" ? item.meta : "", true);
          }
        }
        const firstUser = history.find((item) => item.role === "user");
        if (firstUser) conversationTitle.textContent = firstUser.text.slice(0, 38);
      } else {
        welcome();
      }
    } catch (_) {
      history = [];
      welcome();
    }

    resizePrompt();
    refreshStatus();
    prompt.focus();
  </script>
</body>
</html>)CORTEXHTML";
    return html;
}

} // namespace cortex
