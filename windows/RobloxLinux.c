/*
 * RobloxLinux.exe — instalador e gerenciador do desktop Linux no Windows.
 *
 * Dois modos:
 *   WSL2 (leve):   install, start, stop, status, uninstall
 *   VM (QEMU+WHPX): vm-install, vm-start, vm-stop, vm-status
 *
 * Ambos expõem o desktop em http://localhost:8080. Use um modo por vez.
 *
 * Compilado com C puro e Win32 (sem dependências de runtime).
 */

#define _CRT_RAND_S
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISTRO "Debian"
#define LOCAL_URL "http://localhost:8080/"
#define HEALTH_URL "http://127.0.0.1:8080/health"
#define SHORTCUT_NAME "Roblox Linux.lnk"

/* Modo VM */
#define VM_QEMU_REL "qemu\\qemu-system-x86_64.exe"
#define VM_IMAGE_REL "image\\roblox-linux.qcow2"
#define VM_QMP_PORT 4444
#define VM_RAM_MB 6144
#define VM_CPUS 4
#define VM_BOOT_WAIT_S 420

static char g_exe_path[MAX_PATH];
static char g_exe_dir[MAX_PATH];
static char g_state_dir[MAX_PATH];

/* ---------------------------------------------------------------------------
 * Utilidades
 * ------------------------------------------------------------------------- */

static void trim_right(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static int file_exists(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void open_browser(const char *url) {
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

/*
 * Executa uma linha de comando e espera o término.
 *  - capture = 0: herda o console (saída visível e interativa).
 *  - capture = 1: captura stdout+stderr em `out`.
 * Retorna o código de saída, ou 0xFFFFFFFF em falha de criação.
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

/* Inicia um processo sem esperar e devolve o PID. */
static int spawn_detached(const char *cmdline, DWORD *pid_out) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *buf = _strdup(cmdline);
    BOOL ok;

    if (!buf) return 0;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);

    ok = CreateProcessA(NULL, buf, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi);
    free(buf);
    if (!ok) {
        fprintf(stderr, "Falha ao iniciar o processo (erro %lu)\n", GetLastError());
        return 0;
    }
    *pid_out = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Modo WSL2
 * ------------------------------------------------------------------------- */

/* Executa um comando bash dentro da distro. Não use aspas duplas em `bash_cmd`. */
static DWORD wsl_bash(const char *bash_cmd, int capture, char *out, DWORD out_size) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "wsl.exe -d %s -- bash -lc \"%s\"", DISTRO, bash_cmd);
    return run_process(cmd, capture, out, out_size);
}

static int distro_installed(void) {
    char out[256];
    return wsl_bash("true", 1, out, sizeof out) == 0;
}

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
            "$s.Description = 'Desktop Linux com Roblox';\r\n"
            "$s.Save();\r\n",
            lnk, g_exe_path, g_exe_dir);
    fclose(f);

    snprintf(cmd, sizeof cmd, "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps1);
    {
        DWORD code = run_process(cmd, 1, NULL, 0);
        DeleteFileA(ps1);
        return code == 0;
    }
}

static void remove_start_menu_shortcut(void) {
    char appdata[MAX_PATH], lnk[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", appdata, sizeof appdata) == 0) return;
    snprintf(lnk, sizeof lnk, "%s\\Microsoft\\Windows\\Start Menu\\Programs\\%s", appdata, SHORTCUT_NAME);
    DeleteFileA(lnk);
}

static int cmd_install(void) {
    char check[512];
    char wslenv[1024] = "";
    char existing[1024];

    printf("== Instalação do Desktop Linux (WSL2) ==\n");

    if (run_process("wsl.exe --status", 1, check, sizeof check) != 0) {
        printf("\nO WSL2 ainda não está habilitado. Abrindo a instalação do WSL\n");
        printf("(requer privilégios de administrador e reinicialização).\n");
        run_process("wsl.exe --install --no-distribution", 0, NULL, 0);
        printf("\nReinicie o Windows e execute: RobloxLinux.exe install\n");
        return 1;
    }

    if (!distro_installed()) {
        printf("\nInstalando a distro %s. Na janela que abrir, crie seu usuário Linux.\n", DISTRO);
        printf("Quando terminar, feche a janela e execute novamente: RobloxLinux.exe install\n");
        run_process("wsl.exe --install -d " DISTRO, 0, NULL, 0);
        return 1;
    }

    printf("\nProvisionando o desktop dentro da distro (pode levar vários minutos)...\n");
    SetEnvironmentVariableA("RL_WINDIR", g_exe_dir);
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
        printf("Falha ao iniciar o desktop. Veja ~/.cache/roblox-linux/run.log dentro da distro.\n");
        return 1;
    }

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

/* ---------------------------------------------------------------------------
 * Modo VM (QEMU + WHPX)
 * ------------------------------------------------------------------------- */

static int init_state_dir(void) {
    char local[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof local) == 0) return 0;
    snprintf(g_state_dir, sizeof g_state_dir, "%s\\RobloxLinux", local);
    CreateDirectoryA(g_state_dir, NULL);
    return 1;
}

static void state_path(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s\\%s", g_state_dir, name);
}

/* Token de 48 caracteres hex (24 bytes) gerado uma vez e reaproveitado. `size` >= 49. */
static int ensure_vm_token(char *token, size_t size) {
    char path[MAX_PATH];
    FILE *f;
    unsigned int r;
    size_t i;

    if (size < 49) return 0;
    state_path(path, sizeof path, "vm-token");

    f = fopen(path, "r");
    if (f) {
        int ok = fgets(token, (int)size, f) != NULL;
        fclose(f);
        if (ok) {
            trim_right(token);
            if (strlen(token) >= 16) return 1;
        }
    }

    for (i = 0; i < 12; i++) {
        if (rand_s(&r) != 0) return 0;
        snprintf(token + i * 8, 9, "%08x", r);
    }
    token[48] = '\0';

    f = fopen(path, "w");
    if (!f) return 0;
    fputs(token, f);
    fclose(f);
    return 1;
}

static DWORD read_vm_pid(void) {
    char path[MAX_PATH];
    DWORD pid = 0;
    FILE *f;
    state_path(path, sizeof path, "vm.pid");
    f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%lu", &pid) != 1) pid = 0;
        fclose(f);
    }
    return pid;
}

static void write_vm_pid(DWORD pid) {
    char path[MAX_PATH];
    FILE *f;
    state_path(path, sizeof path, "vm.pid");
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%lu\n", pid);
        fclose(f);
    }
}

static void delete_vm_pid(void) {
    char path[MAX_PATH];
    state_path(path, sizeof path, "vm.pid");
    DeleteFileA(path);
}

/* Retorna um handle com SYNCHRONIZE se o processo ainda existir, senão NULL. */
static HANDLE open_vm_process(DWORD pid) {
    HANDLE h;
    if (pid == 0) return NULL;
    h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    if (!h) return NULL;
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        CloseHandle(h);
        return NULL;
    }
    return h;
}

static int vm_healthy(void) {
    char out[512];
    if (run_process("curl.exe -fsS -m 3 " HEALTH_URL, 1, out, sizeof out) != 0) return 0;
    return strstr(out, "\"desktop\":\"up\"") != NULL;
}

/* Envia desligamento ACPI pelo QMP (socket JSON do QEMU em 127.0.0.1). */
static int vm_powerdown(void) {
    WSADATA wsa;
    SOCKET s;
    struct sockaddr_in addr;
    char buf[512];
    const char *cap = "{\"execute\":\"qmp_capabilities\"}\r\n";
    const char *off = "{\"execute\":\"system_powerdown\"}\r\n";
    int ok = 0;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(VM_QMP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(s, (struct sockaddr *)&addr, sizeof addr) == 0) {
        recv(s, buf, sizeof buf - 1, 0);            /* saudação QMP */
        send(s, cap, (int)strlen(cap), 0);
        recv(s, buf, sizeof buf - 1, 0);
        send(s, off, (int)strlen(off), 0);
        recv(s, buf, sizeof buf - 1, 0);
        ok = 1;
    }

    closesocket(s);
    WSACleanup();
    return ok;
}

static int cmd_vm_install(void) {
    char qemu[MAX_PATH * 2], image[MAX_PATH * 2], token[64];

    if (!init_state_dir()) {
        printf("LOCALAPPDATA não definido.\n");
        return 1;
    }
    snprintf(qemu, sizeof qemu, "%s\\%s", g_exe_dir, VM_QEMU_REL);
    snprintf(image, sizeof image, "%s\\%s", g_exe_dir, VM_IMAGE_REL);

    if (!file_exists(qemu)) {
        printf("QEMU não encontrado: %s\n", qemu);
        printf("Baixe os binários Windows do QEMU e extraia a pasta em 'qemu' ao lado do .exe.\n");
        return 1;
    }
    if (!file_exists(image)) {
        printf("Imagem da VM não encontrada: %s\n", image);
        printf("Gere com vm/build-image.sh (Linux) e copie para a pasta 'image'.\n");
        return 1;
    }
    if (!ensure_vm_token(token, sizeof token)) {
        printf("Falha ao gerar o token.\n");
        return 1;
    }

    printf("VM pronta para iniciar. Execute: RobloxLinux.exe vm-start\n");
    return 0;
}

static int cmd_vm_start(void) {
    char qemu[MAX_PATH * 2], image[MAX_PATH * 2], serial[MAX_PATH * 2];
    char token[64], cmd[4096], url[512];
    DWORD pid = read_vm_pid();
    HANDLE h = NULL;
    int waited = 0;

    if (!init_state_dir()) {
        printf("LOCALAPPDATA não definido.\n");
        return 1;
    }
    snprintf(qemu, sizeof qemu, "%s\\%s", g_exe_dir, VM_QEMU_REL);
    snprintf(image, sizeof image, "%s\\%s", g_exe_dir, VM_IMAGE_REL);
    state_path(serial, sizeof serial, "vm-serial.log");

    if (!file_exists(qemu) || !file_exists(image)) {
        printf("Arquivos da VM ausentes. Execute: RobloxLinux.exe vm-install\n");
        return 1;
    }
    if (!ensure_vm_token(token, sizeof token)) {
        printf("Falha ao preparar o token.\n");
        return 1;
    }

    h = open_vm_process(pid);
    if (h) {
        printf("A VM já está em execução. Aguardando o desktop...\n");
    } else {
        printf("Iniciando a VM (QEMU + WHPX). A primeira inicialização é demorada.\n");
        snprintf(cmd, sizeof cmd,
                 "\"%s\" -name RobloxLinux -machine q35 -accel whpx -accel tcg "
                 "-smp %d -m %d "
                 "-drive file=\"%s\",format=qcow2,if=virtio "
                 "-netdev user,id=n0,hostfwd=tcp:127.0.0.1:8080-:8080 "
                 "-device virtio-net-pci,netdev=n0 "
                 "-vga std -display none "
                 "-serial file:\"%s\" "
                 "-qmp tcp:127.0.0.1:%d,server=on,wait=off "
                 "-fw_cfg name=opt/roblox/token,string=%s",
                 qemu, VM_CPUS, VM_RAM_MB, image, serial, VM_QMP_PORT, token);

        if (!spawn_detached(cmd, &pid)) {
            printf("Não foi possível iniciar o QEMU.\n");
            return 1;
        }
        write_vm_pid(pid);
        h = open_vm_process(pid);
    }

    while (waited < VM_BOOT_WAIT_S) {
        if (vm_healthy()) {
            snprintf(url, sizeof url, "%s#token=%s", LOCAL_URL, token);
            open_browser(url);
            printf("\nDesktop pronto em %s\n", LOCAL_URL);
            if (h) CloseHandle(h);
            return 0;
        }
        if (!h) {
            printf("\nO QEMU encerrou antes de o desktop ficar pronto.\n");
            printf("Veja o log serial em: %s\n", serial);
            delete_vm_pid();
            return 1;
        }
        if (waited % 30 == 0) printf("  aguardando... (%d s)\n", waited);
        Sleep(5000);
        waited += 5;
    }

    printf("\nTempo esgotado. Veja o log serial em: %s\n", serial);
    if (h) CloseHandle(h);
    return 1;
}

static int cmd_vm_stop(void) {
    DWORD pid = read_vm_pid();
    HANDLE h = open_vm_process(pid);

    if (!h) {
        printf("A VM não está em execução.\n");
        delete_vm_pid();
        return 0;
    }

    printf("Desligando a VM...\n");
    if (!vm_powerdown()) {
        printf("Não foi possível falar com o QEMU (QMP). Forçando encerramento.\n");
        TerminateProcess(h, 1);
    } else if (WaitForSingleObject(h, 90000) != WAIT_OBJECT_0) {
        printf("A VM não desligou em 90 s. Forçando encerramento.\n");
        TerminateProcess(h, 1);
    }

    CloseHandle(h);
    delete_vm_pid();
    printf("VM desligada.\n");
    return 0;
}

static int cmd_vm_status(void) {
    DWORD pid = read_vm_pid();
    HANDLE h = open_vm_process(pid);
    if (h) {
        CloseHandle(h);
        printf("VM: em execução (PID %lu)\n", pid);
    } else {
        printf("VM: parada\n");
        return 1;
    }
    printf("Desktop: %s\n", vm_healthy() ? "pronto" : "ainda não respondeu");
    return 0;
}

/* ---------------------------------------------------------------------------
 * CLI
 * ------------------------------------------------------------------------- */

static void usage(void) {
    printf("Uso: RobloxLinux.exe <comando>\n\n");
    printf("Modo WSL2 (leve):\n");
    printf("  install       Instala e provisiona o desktop (WSL2 + Debian)\n");
    printf("  start         Inicia o desktop e abre no navegador\n");
    printf("  stop          Encerra o desktop\n");
    printf("  status        Mostra o estado\n");
    printf("  uninstall     Remove a distro e o atalho\n\n");
    printf("Modo VM (QEMU + WHPX):\n");
    printf("  vm-install    Valida qemu\\ e image\\ e prepara o token\n");
    printf("  vm-start      Inicia a VM e abre o desktop\n");
    printf("  vm-stop       Desliga a VM\n");
    printf("  vm-status     Mostra o estado da VM\n");
}

int main(int argc, char **argv) {
    char *slash;

    (void)argc;
    (void)argv;
    GetModuleFileNameA(NULL, g_exe_path, sizeof g_exe_path);
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", g_exe_path);
    slash = strrchr(g_exe_dir, '\\');
    if (slash) *slash = '\0';

    if (argc < 2) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "install") == 0)    return cmd_install();
    if (strcmp(argv[1], "start") == 0)      return cmd_start();
    if (strcmp(argv[1], "stop") == 0)       return cmd_stop();
    if (strcmp(argv[1], "status") == 0)     return cmd_status();
    if (strcmp(argv[1], "uninstall") == 0)  return cmd_uninstall();
    if (strcmp(argv[1], "vm-install") == 0) return cmd_vm_install();
    if (strcmp(argv[1], "vm-start") == 0)   return cmd_vm_start();
    if (strcmp(argv[1], "vm-stop") == 0)    return cmd_vm_stop();
    if (strcmp(argv[1], "vm-status") == 0)  return cmd_vm_status();

    usage();
    return 1;
}
