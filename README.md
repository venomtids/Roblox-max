# Cloud Linux Desktop & Gaming

Desktop Linux (XFCE4) na nuvem, acessado pelo navegador via noVNC/KasmVNC, com Wine + Vinegar para executar o Roblox.

A arquitetura é dividida em duas partes:

| Parte | Onde roda | Conteúdo |
|---|---|---|
| **Frontend estático** | GitHub Pages (`docs/`) | `index.html`, `app.js`, cliente noVNC 1.7.0 vendorizado |
| **Backend + container** | Render, Railway ou VPS | `Dockerfile`, `entrypoint.sh`, `server.js` (auth + proxy WebSocket), KasmVNC, XFCE4, Mesa, Wine, Vinegar |

```
Navegador ──HTTPS──► GitHub Pages (docs/index.html + app.js)
    │
    └──WSS /websockify?token=...──► Backend (Render/Railway/VPS :$PORT)
                                       │  server.js: valida ACCESS_TOKEN, CORS, rate limit
                                       └──WS 127.0.0.1:6901──► KasmVNC ── XFCE4 ── Wine/Vinegar
```

## Estrutura

```
docs/                     Frontend estático (publicado no GitHub Pages)
  index.html
  app.js
  vendor/novnc/           noVNC 1.7.0 (core/ + vendor/ + LICENSE)
  .nojekyll
server.js                 Gateway Express + proxy WebSocket
package.json / package-lock.json
Dockerfile                Debian bookworm slim + XFCE4 + KasmVNC + Mesa + Wine + Vinegar
entrypoint.sh             Sobe display virtual, KasmVNC, watchdog e gateway
config/vinegar/config.toml.template
render.yaml               Blueprint do Render
railway.json              Configuração do Railway
docker-compose.yml        Execução em VPS
Caddyfile.example         TLS + proxy reverso para VPS
```

---

## 1. Gerar o token de acesso

Use um token longo (mínimo 16 caracteres). Exemplo:

```bash
openssl rand -hex 24
```

Guarde esse valor: ele será a variável `ACCESS_TOKEN` no backend e o que você digita na tela de login.

---

## 2. Publicar o frontend no GitHub Pages

1. No GitHub, abra o repositório `venomtids/Roblox-max` → **Settings → Pages**.
2. Em **Build and deployment → Source**, escolha **Deploy from a branch**.
3. Em **Branch**, selecione `main` (ou a branch que contém a pasta `docs/`) e a pasta **`/docs`**. Clique em **Save**.
4. Aguarde o deploy. A URL será `https://SEU_USUARIO.github.io/Roblox-max/`.

Anote a **origem** (somente `https://SEU_USUARIO.github.io`, sem caminho). Ela vai para `ALLOWED_ORIGINS` no backend.

---

## 3. Subir o backend (container)

### Opção A — Render (recomendada para começar)

1. **New → Blueprint** e selecione este repositório (o `render.yaml` cria o serviço). Ou **New → Web Service → Existing repository → Runtime: Docker**.
2. Em **Environment**, configure:
   - `ACCESS_TOKEN` = o token gerado no passo 1 (o Blueprint pode gerar um automaticamente; copie-o do painel).
   - `ALLOWED_ORIGINS` = `https://SEU_USUARIO.github.io`
3. Em **Instance type**, escolha o plano **Pro (4 GB de RAM)**. Starter e Standard têm memória insuficiente para XFCE + Wine.
4. **Health Check Path**: `/health` (já definido no Blueprint).
5. Deploy. Ao concluir, copie a URL pública, por exemplo `https://cloud-linux-desktop.onrender.com`.

### Opção B — Railway

1. **New Project → Deploy from GitHub repo** e selecione este repositório. O `railway.json` define o build por Dockerfile.
2. Em **Variables**, adicione:
   - `ACCESS_TOKEN` = seu token
   - `ALLOWED_ORIGINS` = `https://SEU_USUARIO.github.io`
3. Em **Settings → Networking**, clique em **Generate Domain**. A porta é detectada automaticamente pela variável `PORT` injetada pelo Railway.
4. Em **Settings → Resources**, dê ao serviço memória suficiente (recomendado 4 GB ou mais).
5. Copie a URL pública `https://xxxx.up.railway.app`.

### Opção C — VPS (Docker Compose + Caddy)

```bash
git clone https://github.com/venomtids/Roblox-max.git && cd Roblox-max
cat > .env <<'EOF'
ACCESS_TOKEN=cole-aqui-o-token-de-16+-caracteres
ALLOWED_ORIGINS=https://SEU_USUARIO.github.io
VNC_RESOLUTION=1920x1080
EOF
docker compose up -d --build
```

Para TLS (obrigatório, pois a página do Pages é HTTPS), instale o Caddy, copie `Caddyfile.example` para `/etc/caddy/Caddyfile`, troque o domínio e execute `sudo systemctl reload caddy`. O Caddy encaminha o WebSocket automaticamente.

---

## 4. Conectar o frontend ao backend

1. Abra `https://SEU_USUARIO.github.io/Roblox-max/`.
2. No campo de endereço, informe a URL do backend, por exemplo `https://cloud-linux-desktop.onrender.com`. O app converte para `wss://…/websockify` automaticamente.
3. Informe o `ACCESS_TOKEN` e clique em **Conectar**.
4. Clique na área do desktop para capturar teclado e mouse. Use **Tela cheia** para expandir; em tela cheia, a tecla Windows/Meta é repassada ao desktop remoto em navegadores Chromium.

Regras importantes:

- Use sempre `https://` (ou `wss://`) no backend. Navegadores bloqueiam `ws://` em páginas HTTPS (*mixed content*), e o app avisa sobre isso.
- Se o app disser "Não foi possível alcançar o backend", confira: URL, serviço ativo, e se `ALLOWED_ORIGINS` contém exatamente a origem do Pages (sem barra final e sem caminho).

---

## 5. Variáveis de ambiente do backend

| Variável | Padrão | Descrição |
|---|---|---|
| `ACCESS_TOKEN` | — (obrigatória) | Token de acesso, mínimo 16 caracteres. O gateway não inicia sem ele. |
| `ALLOWED_ORIGINS` | vazio | Origens permitidas separadas por vírgula, ex.: `https://usuario.github.io`. |
| `PORT` | `8080` | Porta pública. Render e Railway definem automaticamente. |
| `VNC_RESOLUTION` | `1920x1080` | Resolução do desktop virtual. |
| `VNC_DEPTH` | `24` | Profundidade de cor. |
| `VNC_DESKTOP` | `xfce` | Ambiente de desktop. |
| `VNC_PORT` | `6901` | Porta WebSocket interna do KasmVNC (somente loopback). |
| `ROBLOX_AUTOSTART` | `0` | `1` inicia o comando de `ROBLOX_LAUNCH_CMD` ao subir o container. |
| `ROBLOX_LAUNCH_CMD` | `vinegar` | Comando executado no desktop quando `ROBLOX_AUTOSTART=1`. |

### Endpoints do gateway

| Método | Caminho | Auth | Função |
|---|---|---|---|
| GET | `/health` | não | Health check (`{"status":"ok","desktop":"up"}`). |
| POST | `/api/auth/verify` | corpo `{"token":"..."}` | Valida o token. Retorna `200` ou `401`. |
| GET | `/api/status` | `Authorization: Bearer <token>` | Versão, uptime e estado do desktop. |
| WS | `/websockify?token=...` | token na query | Proxy WebSocket para o KasmVNC. |

Após 10 falhas de autenticação em 15 minutos por IP, as tentativas retornam `429`.

---

## 6. Roblox com Wine/Vinegar

- O Vinegar é instalado a partir da tag `v1.9.4` do upstream e compilado no Dockerfile com Go 1.26. Seu configurador fica em `$XDG_CONFIG_HOME/vinegar/config.toml`, criado a partir de `config/vinegar/config.toml.template` na primeira inicialização.
- `wineroot` aponta para `/usr`, onde o Debian instala o Wine.
- O README do upstream descreve o Vinegar como bootstrapper do **Roblox Studio**. Antes de depender dele para o player, confirme o alvo suportado pela versão que você usa. O comando de inicialização é configurável em `ROBLOX_LAUNCH_CMD`.
- Abra o terminal XFCE dentro do desktop remoto para executar o Vinegar manualmente na primeira vez. Ele pedirá sua configuração gráfica.

### Expectativas de desempenho

- **Sem GPU, a renderização é feita pela CPU** (Mesa lavapipe). Isso funciona para validar o ambiente, mas o desempenho em jogos fica muito abaixo do esperado. Para jogos, use uma VPS com GPU (NVIDIA com driver e `nvidia-container-toolkit`) e ajuste a imagem.
- Recomendado: 4 GB de RAM para o desktop, e mais para o Roblox.
- Persistência: o prefixo do Wine e a configuração do Vinegar ficam em `/home/desktop`. No Render, monte um disco persistente nesse caminho (plano pago). Na VPS, o volume `desktop-home` do Compose já cuida disso. Sem disco, o conteúdo é perdido a cada redeploy.

---

## 7. Desenvolvimento local

Frontend (qualquer servidor estático):

```bash
cd docs && python3 -m http.server 8000
```

Backend (sem Docker, requer um KasmVNC rodando em `127.0.0.1:6901` para o proxy de WebSocket):

```bash
npm ci
ACCESS_TOKEN=dev-token-com-16-chars ALLOWED_ORIGINS=http://localhost:8000 PORT=8080 npm start
```

Para testar o container localmente:

```bash
docker build -t cloud-linux-desktop .
docker run --rm -p 8080:8080 --shm-size=1g -e ACCESS_TOKEN=dev-token-com-16-chars \
  -e ALLOWED_ORIGINS=http://localhost:8000 cloud-linux-desktop
curl http://localhost:8080/health
```

Com o frontend em `http://localhost:8000`, use `http://localhost:8080` como endereço. A página é HTTP, então `ws://` é permitido.

---

## 8. Segurança

- Token obrigatório em todas as rotas sensíveis. Comparação em tempo constante (`timingSafeEqual`).
- KasmVNC escuta somente em `127.0.0.1` (`-localhost`), então o socket VNC bruto não fica exposto.
- CORS restrito a `ALLOWED_ORIGINS`. Requisições WebSocket de outras origens recebem `403`.
- Rate limit de falhas por IP.
- O token viaja na query string do WebSocket, porque o navegador não permite cabeçalhos customizados em WebSockets. Use sempre TLS.
- Troque o token se ele vazar. Basta atualizar `ACCESS_TOKEN` e reiniciar o serviço.

## 9. Solução de problemas

| Sintoma | Causa provável | Ação |
|---|---|---|
| Container reinicia no boot | `ACCESS_TOKEN` ausente ou curto | Defina um token com 16+ caracteres. |
| `/health` retorna `"desktop":"down"` | KasmVNC não subiu ou caiu | Veja os logs do container; o `entrypoint.sh` imprime as últimas linhas dos logs do KasmVNC quando a inicialização falha, e o watchdog reinicia o container se o KasmVNC cair. |
| Frontend: "Origem não permitida" | `ALLOWED_ORIGINS` diferente da origem do Pages | Use exatamente `https://usuario.github.io`. |
| Frontend: "Token inválido" | Token digitado diferente do configurado | Confira o valor em `ACCESS_TOKEN`. |
| Tela preta com cursor | Desktop ainda iniciando ou Wine sem display | Aguarde o XFCE e abra o terminal manualmente. |
| Desconexões frequentes | Plano sem recursos ou sleep do plano gratuito | Use plano pago com 4 GB de RAM. |
