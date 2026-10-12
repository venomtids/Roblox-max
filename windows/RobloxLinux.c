/*
 * RobloxLinux.exe — instalador e gerenciador do desktop Linux no Windows (via WSL2).
 *
 * Comandos:
 *   RobloxLinux.exe install     Ativa o WSL2, instala a distro Debian, provisiona o desktop
 *                               (XFCE, KasmVNC, Wine, Vinegar) e cria o atalho no Menu Iniciar.
 *   RobloxLinux.exe start       Inicia o desktop e abre http://localhost:8080 no navegador.
 *   RobloxLinux.exe stop        Encerra o desktop.
 *   RobloxLinux.exe status      Mostra o estado do desktop.
 *   RobloxLinux.exe uninstall   Remove a distro Debian do WSL e o atalho (pede confirmação).
 *
 * Sem dependências de runtime: compilado com C puro e Win32 (user32/shell32 do sistema).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISTRO "Debian"
#define LOCAL_URL "http://localhost:8080/"
#define SHORTCUT_NAME "Roblox Linux.lnk"

static char g_exe_path[MAX_PATH];
static char g_exe_dir[MAX_PATH];

/* ---------------------------------------------------------------------------
 * Utilidades de processo
 * ------------------------------------------------------------------------- */

/*
 * Executa uma linha de comando.
 *  - capture = 0: herda o console (saída visível e interativa).
 *  - capture = 1: captura stdout+stderr em `out` (até out_size - 1 bytes).
 * Retorna o código de saída do processo, ou 0xFFFFFFFF em falha de criação.
 */
static DWORD run_process(const char *cmdline, int capture, char *out, DWORD out_size) {
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL;
    DWORD code = 0xFFFFFFFF;
    char *buf = _strdup(cmdline);

    if (!buf) return code;

    ZeroMemory(&sa, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);

    if (capture) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) goto done;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        if (out && out_size > 0) out[0] = '\0';
    }

    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE,
                        capture ? CREATE_NO_WINDOW : 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "Falha ao iniciar: %s (erro %lu)\n", cmdline, GetLastError());
        goto done;
    }

    if (capture) {
        DWORD n = 0, used = 0;
        CloseHandle(wr);
        wr = NULL;
        while (out && used + 1 < out_size &&
               ReadFile(rd, out + used, out_size - 1 - used, &n, NULL) && n > 0) {
            used += n;
        }
        if (out && out_size > 0) out[used] = '\0';
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

done:
    if (wr) CloseHandle(wr);
    if (rd) CloseHandle(rd);
    free(buf);
    return code;
}

/* Remove espaços e quebras de linha do fim da string. */
static void trim_right(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

/* Executa um comando bash dentro da distro. Não use aspas duplas em `bash_cmd`. */
static DWORD wsl_bash(const char *bash_cmd, int capture, char *out, DWORD out_size) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "wsl.exe -d %s -- bash -lc \"%s\"", DISTRO, bash_cmd);
    return run_process(cmd, capture, out, out_size);
}

static int distro_installed(void) {
    char out[256];
    /* "true" retorna 0 se a distro existe e responde. */
    return wsl_bash("true", 1, out, sizeof out) == 0;
}

static void open_browser(const char *url) {
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

/* ---------------------------------------------------------------------------
 * Atalho no Menu Iniciar (via PowerShell + WScript.Shell, sem dependências)
 * ------------------------------------------------------------------------- */

static int create_start_menu_shortcut(void) {
    char appdata[MAX_PATH], lnk[MAX_PATH], ps1[MAX_PATH], temp[MAX_PATH], cmd[2048];
    FILE *f;
    DWORD n;

    n = GetEnvironmentVariableA("APPDATA", appdata, sizeof appdata);
    if (n == 0 || n >= sizeof appdata) return 0;

    snprintf(lnk, sizeof lnk, "%s\\Microsoft\\Windows\\Start Menu\\Programs\\%s", appdata, SHORTCUT_NAME);

    n = GetTempPathA(sizeof temp, temp);
    if (n == 0 || n >= sizeof temp) return 0;
    snprintf(ps1, sizeof ps1, "%sRobloxLinux-shortcut.ps1", temp);

    f = fopen(ps1, "w");
    if (!f) return 0;
    fprintf(f,
            "$s = (New-Object -ComObject WScript.Shell).CreateShortcut('%s');\r\n"
            "$s.TargetPath = '%s';\r\n"
            "$s.Arguments = 'start';\r\n"
            "$s.WorkingDirectory = '%s';\r\n"
            "$s.Description = 'Desktop Linux com Roblox (WSL2)';\r\n"
            "$s.Save();\r\n",
            lnk, g_exe_path, g_exe_dir);
    fclose(f);

    snprintf(cmd, sizeof cmd,
             "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps1);
    DWORD code = run_process(cmd, 1, NULL, 0);
    DeleteFileA(ps1);
    return code == 0;
}

static void remove_start_menu_shortcut(void) {
    char appdata[MAX_PATH], lnk[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", appdata, sizeof appdata) == 0) return;
    snprintf(lnk, sizeof lnk, "%s\\Microsoft\\Windows\\Start Menu\\Programs\\%s", appdata, SHORTCUT_NAME);
    DeleteFileA(lnk);
}

/* ---------------------------------------------------------------------------
 * Comandos
 * ------------------------------------------------------------------------- */

static int cmd_install(void) {
    char check[512];
    char winpath[MAX_PATH * 2];
    char wslenv[1024] = "";
    char existing[1024];

    printf("== Instalação do Desktop Linux (WSL2) ==\n");

    /* 1. WSL disponível? */
    if (run_process("wsl.exe --status", 1, check, sizeof check) != 0) {
        printf("\nO WSL2 ainda não está habilitado. Abrindo a instalação do WSL\n");
        printf("(requer privilégios de administrador e reinicialização).\n");
        run_process("wsl.exe --install --no-distribution", 0, NULL, 0);
        printf("\nReinicie o Windows e execute: RobloxLinux.exe install\n");
        return 1;
    }

    /* 2. Distro Debian instalada? */
    if (!distro_installed()) {
        printf("\nInstalando a distro %s. Na janela que abrir, crie seu usuário Linux.\n", DISTRO);
        printf("Quando terminar, feche a janela e execute novamente: RobloxLinux.exe install\n");
        run_process("wsl.exe --install -d " DISTRO, 0, NULL, 0);
        return 1;
    }

    /* 3. Provisionamento: passa o caminho do Windows para o WSL via WSLENV (/p = converte caminho). */
    printf("\nProvisionando o desktop dentro da distro (pode levar vários minutos)...\n");
    snprintf(winpath, sizeof winpath, "%s", g_exe_dir);
    SetEnvironmentVariableA("RL_WINDIR", winpath);
    if (GetEnvironmentVariableA("WSLENV", existing, sizeof existing) > 0) {
        snprintf(wslenv, sizeof wslenv, "RL_WINDIR/p:%s", existing);
    } else {
        snprintf(wslenv, sizeof wslenv, "RL_WINDIR/p");
    }
    SetEnvironmentVariableA("WSLENV", wslenv);

    if (wsl_bash("bash \\\"$RL_WINDIR/app/wsl/provision.sh\\\"", 0, NULL, 0) != 0) {
        printf("\nFalha no provisionamento. Veja as mensagens acima e execute novamente.\n");
        return 1;
    }

    /* 4. Atalho no Menu Iniciar */
    if (create_start_menu_shortcut()) {
        printf("\nAtalho criado no Menu Iniciar: %s\n", SHORTCUT_NAME);
    } else {
        printf("\nAviso: não foi possível criar o atalho. Use: RobloxLinux.exe start\n");
    }

    printf("\nInstalação concluída. Para abrir: RobloxLinux.exe start\n");
    return 0;
}

static int cmd_start(void) {
    char token[256];
    char url[512];

    if (!distro_installed()) {
        printf("Desktop não instalado. Execute: RobloxLinux.exe install\n");
        return 1;
    }

    printf("Iniciando o desktop (a primeira vez pode demorar)...\n");
    if (wsl_bash("bash ~/roblox-linux/app/wsl/start.sh", 0, NULL, 0) != 0) {
        printf("Falha ao iniciar o desktop. Veja o log em ~/.cache/roblox-linux/run.log dentro da distro.\n");
        return 1;
    }

    /* Lê o token e abre o navegador com ele no fragmento (#token=), que não é enviado ao servidor. */
    if (wsl_bash("cat ~/.config/roblox-linux/token", 1, token, sizeof token) != 0) {
        printf("Não foi possível ler o token.\n");
        return 1;
    }
    trim_right(token);
    snprintf(url, sizeof url, "%s#token=%s", LOCAL_URL, token);
    open_browser(url);
    printf("Desktop aberto em %s\n", LOCAL_URL);
    return 0;
}

static int cmd_stop(void) {
    if (!distro_installed()) {
        printf("Desktop não instalado.\n");
        return 1;
    }
    return wsl_bash("bash ~/roblox-linux/app/wsl/stop.sh", 0, NULL, 0) == 0 ? 0 : 1;
}

static int cmd_status(void) {
    char out[512];
    if (!distro_installed()) {
        printf("Desktop não instalado.\n");
        return 1;
    }
    if (wsl_bash("curl -fsS -m 3 http://127.0.0.1:8080/health || echo 'offline'", 1, out, sizeof out) != 0) {
        printf("Não foi possível consultar o status.\n");
        return 1;
    }
    trim_right(out);
    printf("%s\n", out);
    return 0;
}

static int cmd_uninstall(void) {
    char answer[16] = "";
    printf("Isto remove a distro %s do WSL, incluindo o desktop, o Wine e os dados.\n", DISTRO);
    printf("Digite SIM para confirmar: ");
    if (!fgets(answer, sizeof answer, stdin)) return 1;
    trim_right(answer);
    if (strcmp(answer, "SIM") != 0) {
        printf("Cancelado.\n");
        return 0;
    }
    wsl_bash("bash ~/roblox-linux/app/wsl/stop.sh", 0, NULL, 0);
    run_process("wsl.exe --unregister " DISTRO, 0, NULL, 0);
    remove_start_menu_shortcut();
    printf("Desinstalado.\n");
    return 0;
}

static void usage(void) {
    printf("Uso: RobloxLinux.exe <comando>\n\n");
    printf("  install     Instala e provisiona o desktop Linux (WSL2 + Debian)\n");
    printf("  start       Inicia o desktop e abre no navegador\n");
    printf("  stop        Encerra o desktop\n");
    printf("  status      Mostra o estado do desktop\n");
    printf("  uninstall   Remove a distro e o atalho\n");
}

int main(int argc, char **argv) {
    char *slash;

    GetModuleFileNameA(NULL, g_exe_path, sizeof g_exe_path);
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", g_exe_path);
    slash = strrchr(g_exe_dir, '\\');
    if (slash) *slash = '\0';

    if (argc < 2) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "install") == 0)   return cmd_install();
    if (strcmp(argv[1], "start") == 0)     return cmd_start();
    if (strcmp(argv[1], "stop") == 0)      return cmd_stop();
    if (strcmp(argv[1], "status") == 0)    return cmd_status();
    if (strcmp(argv[1], "uninstall") == 0) return cmd_uninstall();

    usage();
    return 1;
}
