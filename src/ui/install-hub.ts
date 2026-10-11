/**
 * ============================================================================
 *  CENTRAL DE INSTALAÇÃO — o caminho que REALMENTE instala o Roblox
 * ============================================================================
 *  Três perguntas que este módulo responde, no site, para a máquina do usuário:
 *
 *   1) "Como eu instalo o Roblox AGORA?"  → passo a passo pelo sistema detectado
 *   2) "Dá para rodar Linux dentro do navegador e instalar o Roblox nele?"
 *      → Sim para o Linux; NÃO para o Roblox. E o porquê, item por item,
 *        com os impedimentos técnicos específicos (não é opinião).
 *   3) "E se eu não quiser instalar nada?" → o motor deste próprio site
 *      (arquivo único, duplo clique) e o instalador Linux quando aplicável.
 *
 *  Toda a lógica de decisão é PURA (funções de UA/gênero → plano), por isso é
 *  testável em Node sem navegador — ver tools/install-hub-check.ts.
 */

export type Platform = 'windows' | 'macos' | 'linux' | 'steamdeck' | 'chromeos' | 'android' | 'ios' | 'unknown';

export interface InstallStep {
  title: string;
  detail: string;
  /** Comando para copiar (Linux). */
  code?: string;
  /** Link oficial para abrir. */
  link?: string;
  /** Aviso curto para o passo (ex.: "precisa de senha"). */
  warn?: string;
}

export interface InstallPlan {
  platform: Platform;
  badge: string;
  headline: string;
  summary: string;
  steps: InstallStep[];
  alternatives: { label: string; href: string; detail: string }[];
}

/** Impedimento técnico — cada um é veredito de engenharia, não preferência. */
export interface Blocker {
  title: string;
  detail: string;
  severity: 'impossível' | 'proibitivo';
}

/**
 * POR QUE O ROBLOX NÃO RODA DENTRO DE UM "LINUX NO NAVEGADOR".
 * Estes quatro impedimentos são independentes: mesmo resolvendo um, os outros
 * continuam bloqueando. É por isso que não existe (e não vai existir por esse
 * caminho) Roblox dentro de uma VM de navegador.
 */
export const BROWSER_VM_BLOCKERS: Blocker[] = [
  {
    severity: 'impossível',
    title: '32-bit × x86-64 — incompatibilidade de arquitetura',
    detail:
      'Emuladores de PC em WebAssembly que funcionam hoje (v86, o mais maduro) emulam um PC de 32 bits (i386, nível Pentium). '
      + 'O Roblox de Linux só existe no caminho Android, e o APK é x86-64 — exige o modo longo de 64 bits, que o v86 não implementa. '
      + 'Não é lentidão: é a instrução não existir. (WebVM/CheerpX faz userland x86-64, mas continua sem GPU e sem kernel — outro beco.)',
  },
  {
    severity: 'impossível',
    title: 'Sem GPU: não há Vulkan nem OpenGL ES no emulador',
    detail:
      'A VM expõe um adaptador VGA emulado (um framebuffer por software). O runtime do Roblox para Linux exige Vulkan 1.0+ ou OpenGL ES 3.0 — '
      + 'e não existe passagem de WebGPU para dentro de uma VM de navegador. Sem driver gráfico, o cliente nem inicializa.',
  },
  {
    severity: 'impossível',
    title: 'Anti-cheat Hyperion e os Termos de Uso',
    detail:
      'A Roblox bloqueou Wine de propósito (27/02/2024) porque a emulação era usada para trapaça. '
      + 'Rodar o cliente dentro de uma máquina virtual no navegador é exatamente essa categoria: seria detectado/recusado, e a conta pode ser banida. '
      + 'Não construo nem incentivo caminho de contorno — isso derrubaria a conta e o projeto.',
  },
  {
    severity: 'proibitivo',
    title: 'Desempenho: 1–5% do nativo',
    detail:
      'Um emulador de sistema completo em WebAssembly entrega ~1–5% da velocidade da máquina. Mesmo com GPU e 64 bits resolvidos, '
      + 'o Roblox rodaria a menos de 1 FPS: o carregamento de assets, a física e o loop de render não fecham 60 Hz nem perto.',
  },
];

/** Detecta a plataforma pelo User-Agent — função pura para ser testável. */
export function platformFromUA(ua: string): Platform {
  const u = ua.toLowerCase();
  // Steam Deck se identifica como Linux + "Valve Steam Deck" no UA
  if (/valve steam deck|steamdeck/.test(u)) return 'steamdeck';
  if (/windows|win32|win64/.test(u)) return 'windows';
  if (/cros /.test(u)) return 'chromeos';
  if (/android/.test(u)) return 'android';
  if (/iphone|ipad|ipod/.test(u)) return 'ios';
  if (/mac os x|macintosh/.test(u)) return 'macos';
  if (/linux|x11|ubuntu|fedora|debian|arch/.test(u)) return 'linux';
  return 'unknown';
}

const ROBLOX_DOWNLOAD = 'https://www.roblox.com/download';
const LINUX_SCRIPT = '/downloads/roblox-linux.sh';
const STANDALONE = '/downloads/roblox-max.html';

/**
 * Plano de instalação por plataforma. `distro` é opcional e só refina o texto
 * (o script Linux detecta a distro sozinho de qualquer forma).
 */
export function planFor(platform: Platform, distro = ''): InstallPlan {
  const alternatives = [
    { label: 'Jogar sem instalar nada (motor deste site)', href: STANDALONE, detail: 'Arquivo único de ~123 KB, duplo clique, offline' },
    { label: 'Instalador de Linux (Sober/Flatpak)', href: LINUX_SCRIPT, detail: 'Script de 34 KB com diagnóstico do sistema' },
  ];

  switch (platform) {
    case 'windows':
      return {
        platform, badge: 'Windows detectado',
        headline: 'Você instala o Roblox em 2 minutos — é o caminho oficial',
        summary: 'No Windows o Roblox é um aplicativo suportado oficialmente. Não precisa de Linux, VM, nem nada deste projeto.',
        steps: [
          { title: '1. Baixe o instalador oficial', detail: 'Apenas do site oficial da Roblox — evite "instaladores" de terceiros.', link: ROBLOX_DOWNLOAD },
          { title: '2. Execute o download', detail: 'O arquivo RobloxPlayerInstaller.exe baixa e instala o cliente. Se o Windows SmartScreen avisar, clique em "Mais informações → Executar assim mesmo" (é o instalador assinado da Roblox).' },
          { title: '3. Faça login e jogue', detail: 'O Roblox abre no navegador (roblox.com) para escolher a experiência; o cliente é chamado automaticamente.' },
          { title: '4. Se o anti-cheat reclamar', detail: 'O Hyperion exige que o jogo rode fora de VMs e com o Hyper-V/VBS compatível. Em caso de erro "Hyperion", atualize o Windows e reinicie.', warn: 'Não use versões modificadas ("FPS unlockers" piratas): risco real de ban.' },
        ],
        alternatives,
      };
    case 'macos':
      return {
        platform, badge: 'macOS detectado',
        headline: 'No macOS use o cliente oficial',
        summary: 'A Roblox publica cliente para macOS — o caminho é o mesmo do Windows.',
        steps: [
          { title: '1. Baixe o cliente oficial', detail: 'Instalador .dmg assinado pela Roblox.', link: ROBLOX_DOWNLOAD },
          { title: '2. Abra o .dmg e arraste o Roblox para Aplicativos', detail: 'Na primeira execução, autorize em Ajustes → Privacidade e Segurança.' },
          { title: '3. Entre e jogue', detail: 'Login pelo cliente ou pelo site.' },
        ],
        alternatives,
      };
    case 'linux':
      return {
        platform, badge: distro ? `Linux detectado (${distro})` : 'Linux detectado',
        headline: 'No Linux existe UM caminho que funciona: o Sober',
        summary: 'O Wine foi bloqueado de propósito pelo anti-cheat Hyperion em 27/02/2024. O que funciona é o Sober, que executa o APK oficial do Roblox (Android). O script abaixo instala e diagnostica tudo.',
        steps: [
          { title: '1. Baixe o instalador', detail: 'Script auditável de 34 KB — sem curl|bash, sem binário de terceiros.', link: LINUX_SCRIPT },
          { title: '2. Dê permissão e rode o diagnóstico', detail: 'O doctor diz exatamente o que falta na SUA distro, com o comando de correção.', code: 'chmod +x roblox-linux.sh\n./roblox-linux.sh doctor' },
          { title: '3. Instale', detail: 'Instala Flatpak + Flathub + drivers Vulkan + Sober + atalho no menu. Dá para simular antes com --dry-run.', code: './roblox-linux.sh --dry-run install   # simula\n./roblox-linux.sh install            # instala' },
          { title: '4. Primeira execução', detail: 'Abra "Roblox" no menu → escolha Automatic (~400 MB) → faça login → jogue.', warn: 'Não use Wine/Proton/Vinegar/Grapejuice para o Player: não funcionam desde 2024.' },
        ],
        alternatives,
      };
    case 'steamdeck':
      return {
        platform, badge: 'Steam Deck detectado',
        headline: 'No Steam Deck: Modo Desktop para instalar, Modo Jogo para jogar',
        summary: 'O Flatpak já vem pronto no SteamOS. Instale o Sober no Modo Desktop e adicione como jogo não-Steam.',
        steps: [
          { title: '1. Vá para o Modo Desktop', detail: 'Botão STEAM → Energia → Alternar para Área de Trabalho.' },
          { title: '2. Instale o Sober', detail: 'Baixe o script e rode o install (ele detecta sistema imutável e usa só o escopo do usuário, sem sudo).', link: LINUX_SCRIPT },
          { title: '3. Volte ao Modo Jogo e adicione', detail: 'Adicionar jogo não-Steam → Navegar → /usr/bin/flatpak (ou use o atalho "Roblox" criado no menu). Se o Vulkan falhar, atualize o SteamOS.' },
        ],
        alternatives,
      };
    case 'chromeos':
      return {
        platform, badge: 'ChromeOS detectado',
        headline: 'No ChromeOS use o app Android do Roblox',
        summary: 'ChromeOS roda aplicativos Android: é o caminho mais direto e sem gambiarra.',
        steps: [
          { title: '1. Abra o Google Play na loja do Chromebook', detail: 'Configurações → Aplicativos → Google Play (ative se estiver desativado).' },
          { title: '2. Instale o "Roblox"', detail: 'Aplicativo oficial da Roblox Corporation.' },
          { title: '3. Alternativa: Linux (Crostini)', detail: 'Chromebooks com Linux ativado podem rodar o instalador Sober, mas exigem GPU compatível com Vulkan.', link: LINUX_SCRIPT },
        ],
        alternatives,
      };
    case 'android':
      return {
        platform, badge: 'Android detectado',
        headline: 'No Android baixe o app oficial',
        summary: 'Plataforma oficialmente suportada — nada a instalar além do app.',
        steps: [
          { title: '1. Google Play → "Roblox"', detail: 'Aplicativo oficial (Roblox Corporation). Não instale APKs de sites aleatórios.' },
          { title: '2. Entre e jogue', detail: 'No mobile o Roblox já usa o mesmo APK que o Sober executa no Linux.' },
        ],
        alternatives,
      };
    case 'ios':
      return {
        platform, badge: 'iPhone/iPad detectado',
        headline: 'No iOS use a App Store',
        summary: 'Plataforma oficialmente suportada.',
        steps: [{ title: '1. App Store → "Roblox"', detail: 'Aplicativo oficial da Roblox Corporation.' }],
        alternatives,
      };
    default:
      return {
        platform: 'unknown', badge: 'Sistema não identificado',
        headline: 'Escolha o seu sistema',
        summary: 'Não consegui identificar o sistema pelo navegador. Os caminhos oficiais são:',
        steps: [
          { title: 'Windows ou macOS', detail: 'Baixe o cliente oficial direto da Roblox.', link: ROBLOX_DOWNLOAD },
          { title: 'Linux', detail: 'Use o instalador Sober deste projeto (é o único caminho que funciona hoje).', link: LINUX_SCRIPT },
          { title: 'Android / iOS / ChromeOS', detail: 'Instale o app oficial pela loja do sistema.' },
        ],
        alternatives,
      };
  }
}

/**
 * PAINEL DO SITE — pinta a Central de Instalação com Tailwind.
 * Sem framework, sem reconciliação: monta o DOM uma vez e alterna visibilidade.
 */
export class InstallHub {
  private overlay: HTMLDivElement;
  private built = false;

  constructor(private host: HTMLElement = document.body) {
    this.overlay = document.createElement('div');
    this.overlay.id = 'install-hub';
    this.overlay.className = 'fixed inset-0 z-50 hidden items-center justify-center bg-black/75 backdrop-blur-sm p-4';
    this.host.appendChild(this.overlay);
    this.overlay.addEventListener('click', (e) => { if (e.target === this.overlay) this.close(); });
    addEventListener('keydown', (e: KeyboardEvent) => { if (e.key === 'Escape' && this.isOpen()) this.close(); });
  }

  isOpen(): boolean { return this.overlay.dataset.open === '1'; }
  open(): void { if (!this.built) this.build(); this.overlay.dataset.open = '1'; this.overlay.classList.remove('hidden'); this.overlay.classList.add('flex'); }
  close(): void { this.overlay.dataset.open = '0'; this.overlay.classList.add('hidden'); this.overlay.classList.remove('flex'); }
  toggle(): void { this.isOpen() ? this.close() : this.open(); }

  private build(): void {
    this.built = true;
    const plan = planFor(platformFromUA(navigator.userAgent));
    const esc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

    this.overlay.innerHTML = `
      <div class="panel w-[min(96vw,880px)] max-h-[92vh] overflow-auto p-6">
        <div class="flex items-start justify-between gap-4">
          <div>
            <div class="chip border-vx-accent/40 text-vx-accent">${esc(plan.badge)}</div>
            <h2 class="mt-2 text-2xl font-bold tracking-wide">${esc(plan.headline)}</h2>
            <p class="mt-2 max-w-[62ch] text-sm text-slate-400">${esc(plan.summary)}</p>
          </div>
          <button id="ih-close" class="btn shrink-0">Fechar</button>
        </div>

        <div class="mt-6 grid gap-3">
          ${plan.steps.map((s) => `
            <div class="rounded-xl border border-vx-line bg-black/30 p-4">
              <div class="flex items-start justify-between gap-3">
                <div class="font-semibold text-slate-100">${esc(s.title)}</div>
                ${s.link ? `<a class="btn shrink-0" href="${s.link}" target="_blank" rel="noreferrer">abrir ↗</a>` : ''}
              </div>
              <p class="mt-1 text-[13px] leading-relaxed text-slate-400">${esc(s.detail)}</p>
              ${s.code ? `<pre class="mt-2 overflow-auto rounded-lg border border-vx-line bg-black/60 p-3 font-mono text-[12px] text-vx-accent">${esc(s.code)}</pre>` : ''}
              ${s.warn ? `<p class="mt-2 text-[12px] text-vx-warn">⚠ ${esc(s.warn)}</p>` : ''}
            </div>`).join('')}
        </div>

        <div class="mt-6 rounded-xl border border-vx-danger/40 bg-vx-danger/5 p-4">
          <h3 class="text-sm font-bold uppercase tracking-widest text-vx-danger">
            Linux dentro do navegador: funciona. Roblox dentro dele: não.
          </h3>
          <p class="mt-2 text-[13px] text-slate-400">
            Linux no navegador existe (v86, WebVM/CheerpX, anuraOS) e é impressionante. Mas o Roblox
            <b class="text-slate-200">não roda lá dentro</b> — por quatro motivos independentes. Qualquer um deles basta:
          </p>
          <div class="mt-4 grid gap-3 sm:grid-cols-2">
            ${BROWSER_VM_BLOCKERS.map((b) => `
              <div class="rounded-lg border border-vx-line bg-black/40 p-3">
                <div class="flex items-center gap-2">
                  <span class="chip ${b.severity === 'impossível' ? 'border-vx-danger/50 text-vx-danger' : 'border-vx-warn/50 text-vx-warn'}">${b.severity}</span>
                  <span class="text-[13px] font-semibold text-slate-200">${esc(b.title)}</span>
                </div>
                <p class="mt-2 text-[12px] leading-relaxed text-slate-400">${esc(b.detail)}</p>
              </div>`).join('')}
          </div>
        </div>

        <div class="mt-6">
          <h3 class="text-sm font-bold uppercase tracking-widest text-slate-300">Alternativas prontas neste projeto</h3>
          <div class="mt-3 grid gap-3 sm:grid-cols-2">
            ${plan.alternatives.map((a) => `
              <a class="rounded-xl border border-vx-line bg-black/30 p-4 transition hover:border-vx-accent/50" href="${a.href}" download>
                <div class="text-[13px] font-semibold text-vx-accent">${esc(a.label)}</div>
                <div class="mt-1 text-[12px] text-slate-400">${esc(a.detail)}</div>
              </a>`).join('')}
          </div>
        </div>

        <p class="mt-5 text-[11px] text-slate-500">
          Este site não distribui nem modifica o cliente do Roblox. Os links acima apontam para os canais oficiais
          da Roblox e para ferramentas deste repositório. O instalador de Linux usa o Sober (Flatpak do Flathub),
          que executa o APK oficial do Roblox baixado dos servidores oficiais.
        </p>
      </div>`;

    this.overlay.querySelector('#ih-close')?.addEventListener('click', () => this.close());
  }
}
