#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <wininet.h>
#include <wintrust.h>
#include <softpub.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define APP_TITLE L"Roblox Bootstrapper Advanced"
#define ID_PLACE 101
#define ID_PRIVATE 102
#define ID_LAUNCH 103
#define ID_KEEP_OPEN 104
#define ID_DIAGNOSE 105
#define ID_REPAIR 106
#define ID_CLEAR_CACHE 107
#define ID_OPEN_LOG 108
#define WATCHDOG_TIMER 1
#define MAX_AUTOMATIC_RESTARTS 3
#define WM_TOOL_STATUS (WM_APP + 1)
#define WM_TOOL_DONE (WM_APP + 2)
#define TOOL_REPAIR 1
#define TOOL_CACHE 2
#define ERR_DOWNLOAD 0xE001
#define ERR_SIGNATURE 0xE002
#define ERR_INSTALLER 0xE003

static HWND g_window, g_place, g_private, g_status, g_log, g_launch, g_keep_open;
static HWND g_diagnose, g_repair, g_cache, g_open_log;
static wchar_t g_last_uri[1400];
static BOOL g_tracking = FALSE, g_seen_roblox = FALSE;
static ULONGLONG g_last_launch_time = 0;
static int g_restart_count = 0;
static HFONT g_font, g_bold, g_title;
static HBRUSH g_background_brush, g_input_brush;
static COLORREF g_background, g_surface, g_text, g_muted, g_primary;

static BOOL system_uses_dark_mode(void) {
    DWORD value = 1, size = sizeof(value);
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL, (BYTE*)&value, &size);
        RegCloseKey(key);
    }
    return value == 0;
}

static void set_font(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

static HWND create_label(const wchar_t *text, int x, int y, int width, int height,
                         DWORD style, HFONT font) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, g_window, NULL, GetModuleHandleW(NULL), NULL);
    set_font(label, font);
    return label;
}

static HWND create_input(const wchar_t *placeholder, int id, int x, int y, int width, int height) {
    HWND input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        x, y, width, height, g_window, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    set_font(input, g_font);
    SendMessageW(input, EM_SETCUEBANNER, TRUE, (LPARAM)placeholder);
    return input;
}

static void rounded_region(HWND control, int width, int height, int radius) {
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    SetWindowRgn(control, region, TRUE);
}

static BOOL valid_place_id(const wchar_t *value) {
    size_t length = wcslen(value);
    if (length == 0 || length > 20) return FALSE;
    for (size_t i = 0; i < length; ++i)
        if (value[i] < L'0' || value[i] > L'9') return FALSE;
    return TRUE;
}

static BOOL valid_code(const wchar_t *value) {
    size_t length = wcslen(value);
    if (length < 8 || length > 128) return FALSE;
    for (size_t i = 0; i < length; ++i) {
        wchar_t c = value[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'_' || c == L'-')) return FALSE;
    }
    return TRUE;
}

static BOOL official_https_url(const wchar_t *url) {
    const wchar_t *prefixes[] = {
        L"https://www.roblox.com/", L"https://roblox.com/", L"https://web.roblox.com/"
    };
    for (int i = 0; i < 3; ++i)
        if (_wcsnicmp(url, prefixes[i], wcslen(prefixes[i])) == 0) return TRUE;
    return FALSE;
}

static BOOL query_value(const wchar_t *url, const wchar_t *name,
                        wchar_t *output, size_t output_size) {
    const wchar_t *query = wcschr(url, L'?');
    if (!query) return FALSE;
    ++query;
    size_t name_length = wcslen(name);
    while (*query) {
        const wchar_t *item_end = wcschr(query, L'&');
        if (!item_end) item_end = query + wcslen(query);
        const wchar_t *equals = wcschr(query, L'=');
        if (equals && equals < item_end && (size_t)(equals - query) == name_length &&
            _wcsnicmp(query, name, name_length) == 0) {
            size_t length = (size_t)(item_end - equals - 1);
            if (length == 0 || length >= output_size) return FALSE;
            wcsncpy(output, equals + 1, length);
            output[length] = 0;
            return TRUE;
        }
        query = *item_end ? item_end + 1 : item_end;
    }
    return FALSE;
}

static BOOL place_from_game_url(const wchar_t *url, wchar_t *output, size_t output_size) {
    const wchar_t *games = wcsstr(url, L"/games/");
    if (!games) return FALSE;
    games += 7;
    size_t length = 0;
    while (games[length] >= L'0' && games[length] <= L'9') ++length;
    if (length == 0 || length >= output_size) return FALSE;
    wcsncpy(output, games, length);
    output[length] = 0;
    return valid_place_id(output);
}

static void append_log(const wchar_t *message) {
    SYSTEMTIME time;
    GetLocalTime(&time);
    wchar_t old_text[4096], new_text[4600];
    GetWindowTextW(g_log, old_text, 4096);
    swprintf(new_text, 4600, L"%ls[%02u:%02u:%02u] %ls\r\n",
             old_text, time.wHour, time.wMinute, time.wSecond, message);
    SetWindowTextW(g_log, new_text);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void set_status(const wchar_t *text) {
    SetWindowTextW(g_status, text);
}

static BOOL roblox_is_running(void) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return FALSE;
    PROCESSENTRY32W entry = { sizeof(entry) };
    BOOL found = FALSE;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"RobloxPlayerBeta.exe") == 0) {
                found = TRUE;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

static BOOL dispatch_uri(const wchar_t *uri) {
    HINSTANCE result = ShellExecuteW(g_window, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) return FALSE;
    g_last_launch_time = GetTickCount64();
    return TRUE;
}

static void disable_watchdog(const wchar_t *reason) {
    SendMessageW(g_keep_open, BM_SETCHECK, BST_UNCHECKED, 0);
    g_tracking = FALSE;
    g_seen_roblox = FALSE;
    set_status(L"Reabertura automática desativada");
    append_log(reason);
}

static void monitor_roblox(void) {
    if (!g_tracking || SendMessageW(g_keep_open, BM_GETCHECK, 0, 0) != BST_CHECKED || !g_last_uri[0])
        return;

    BOOL running = roblox_is_running();
    if (running) {
        if (!g_seen_roblox) append_log(L"Roblox detectado. Monitoramento opcional ativo.");
        g_seen_roblox = TRUE;
        return;
    }

    ULONGLONG now = GetTickCount64();
    /* Aguarda a inicialização lenta para não abrir instâncias duplicadas. */
    if (!g_seen_roblox && now - g_last_launch_time < 15000) return;
    if (g_restart_count >= MAX_AUTOMATIC_RESTARTS) {
        disable_watchdog(L"Proteção contra loop: limite de 3 reaberturas atingido. Opção desligada.");
        MessageBoxW(g_window,
            L"O Roblox fechou repetidamente. A reabertura automática foi desativada para evitar um loop e consumo excessivo. Use o reparador e verifique os logs do Roblox.",
            APP_TITLE, MB_OK | MB_ICONWARNING);
        return;
    }

    ++g_restart_count;
    g_seen_roblox = FALSE;
    set_status(L"Roblox fechou; reabrindo...");
    append_log(L"Roblox não está em execução. Fazendo uma tentativa controlada.");
    if (!dispatch_uri(g_last_uri)) {
        disable_watchdog(L"O protocolo roblox: falhou durante a reabertura.");
    }
}

static BOOL clear_directory(const wchar_t *directory) {
    wchar_t pattern[2 * MAX_PATH], path[2 * MAX_PATH];
    swprintf(pattern, 2 * MAX_PATH, L"%ls\\*", directory);
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    BOOL success = TRUE;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        swprintf(path, 2 * MAX_PATH, L"%ls\\%ls", directory, data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!clear_directory(path)) success = FALSE;
            SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
            if (!RemoveDirectoryW(path)) success = FALSE;
        } else {
            SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(path)) success = FALSE;
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return success;
}

static void stop_roblox_processes(void) {
    const wchar_t *targets[] = { L"RobloxPlayerBeta.exe", L"RobloxCrashHandler.exe" };
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry = { sizeof(entry) };
    if (Process32FirstW(snapshot, &entry)) {
        do {
            for (int i = 0; i < 2; ++i) {
                if (_wcsicmp(entry.szExeFile, targets[i]) == 0) {
                    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
                    if (process) {
                        TerminateProcess(process, 0);
                        WaitForSingleObject(process, 3000);
                        CloseHandle(process);
                    }
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

static BOOL verify_authenticode(const wchar_t *path) {
    WINTRUST_FILE_INFO file = { sizeof(file) };
    file.pcwszFilePath = path;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA trust = { sizeof(trust) };
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG status = WinVerifyTrust(NULL, &policy, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &policy, &trust);
    return status == ERROR_SUCCESS;
}

static DWORD repair_official_roblox(void) {
    static const wchar_t *metadata_url =
        L"https://clientsettingscdn.roblox.com/v2/client-version/WindowsPlayer/channel/LIVE";
    wchar_t temp[MAX_PATH], metadata[2 * MAX_PATH], installer[2 * MAX_PATH];
    wchar_t installer_url[512], version_wide[96];
    char json[8192] = {0}, version[96] = {0};
    if (!GetTempPathW(MAX_PATH, temp)) return ERR_DOWNLOAD;
    swprintf(metadata, 2 * MAX_PATH, L"%lsRoblox-versao-oficial.json", temp);
    swprintf(installer, 2 * MAX_PATH, L"%lsRobloxPlayerInstaller-oficial.exe", temp);
    DeleteFileW(metadata); DeleteFileW(installer); DeleteUrlCacheEntryW(metadata_url);
    PostMessageW(g_window, WM_TOOL_STATUS, 0, (LPARAM)L"Consultando versão oficial...");
    if (FAILED(URLDownloadToFileW(NULL, metadata_url, metadata, 0, NULL))) return ERR_DOWNLOAD;

    HANDLE file = CreateFileW(metadata, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { DeleteFileW(metadata); return ERR_DOWNLOAD; }
    DWORD read = 0;
    ReadFile(file, json, sizeof(json) - 1, &read, NULL);
    CloseHandle(file); DeleteFileW(metadata);
    const char *key = "\"clientVersionUpload\":\"";
    char *start = strstr(json, key);
    if (!start) return ERR_DOWNLOAD;
    start += strlen(key);
    char *end = strchr(start, '"');
    size_t length = end ? (size_t)(end - start) : 0;
    if (length < 9 || length >= sizeof(version)) return ERR_DOWNLOAD;
    for (size_t i = 0; i < length; ++i) {
        char c = start[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return ERR_DOWNLOAD;
        version[i] = c;
    }
    version[length] = 0;
    if (strncmp(version, "version-", 8) != 0) return ERR_DOWNLOAD;
    MultiByteToWideChar(CP_UTF8, 0, version, -1, version_wide, 96);
    swprintf(installer_url, 512,
        L"https://setup.rbxcdn.com/%ls-RobloxPlayerInstaller.exe", version_wide);
    DeleteUrlCacheEntryW(installer_url);
    PostMessageW(g_window, WM_TOOL_STATUS, 0, (LPARAM)L"Baixando instalador oficial...");
    if (FAILED(URLDownloadToFileW(NULL, installer_url, installer, 0, NULL))) return ERR_DOWNLOAD;

    file = CreateFileW(installer, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BYTE header[2] = {0}; read = 0;
    if (file != INVALID_HANDLE_VALUE) { ReadFile(file, header, 2, &read, NULL); CloseHandle(file); }
    PostMessageW(g_window, WM_TOOL_STATUS, 0, (LPARAM)L"Validando assinatura digital...");
    if (read != 2 || header[0] != 'M' || header[1] != 'Z' || !verify_authenticode(installer)) {
        DeleteFileW(installer);
        return ERR_SIGNATURE;
    }

    PostMessageW(g_window, WM_TOOL_STATUS, 0, (LPARAM)L"Removendo arquivos corrompidos...");
    wchar_t local[MAX_PATH], path[2 * MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    stop_roblox_processes();
    swprintf(path, 2 * MAX_PATH, L"%ls\\Roblox", local);
    clear_directory(path); RemoveDirectoryW(path);

    PostMessageW(g_window, WM_TOOL_STATUS, 0, (LPARAM)L"Executando instalador oficial...");
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = g_window;
    info.lpVerb = L"open";
    info.lpFile = installer;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) { DeleteFileW(installer); return ERR_INSTALLER; }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess); DeleteFileW(installer);
    return code == 0 ? 0 : ERR_INSTALLER;
}

static DWORD clear_roblox_cache(void) {
    wchar_t local[MAX_PATH], temp[MAX_PATH], path[2 * MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    GetEnvironmentVariableW(L"TEMP", temp, MAX_PATH);
    const wchar_t *folders[] = { L"Downloads", L"logs", L"http", L"LocalStorage\\roblox" };
    DWORD result = 0;
    for (int i = 0; i < 4; ++i) {
        swprintf(path, 2 * MAX_PATH, L"%ls\\Roblox\\%ls", local, folders[i]);
        if (!clear_directory(path)) result = 1;
    }
    swprintf(path, 2 * MAX_PATH, L"%ls\\Roblox", temp);
    if (!clear_directory(path)) result = 1;
    return result;
}

static DWORD WINAPI tool_worker(LPVOID parameter) {
    int tool = (int)(INT_PTR)parameter;
    DWORD result = tool == TOOL_REPAIR ? repair_official_roblox() : clear_roblox_cache();
    PostMessageW(g_window, WM_TOOL_DONE, (WPARAM)result, (LPARAM)tool);
    return 0;
}

static void enable_tools(BOOL enabled) {
    EnableWindow(g_launch, enabled);
    EnableWindow(g_diagnose, enabled);
    EnableWindow(g_repair, enabled);
    EnableWindow(g_cache, enabled);
    EnableWindow(g_open_log, enabled);
}

static void start_tool(int tool) {
    enable_tools(FALSE);
    set_status(tool == TOOL_REPAIR ? L"Preparando reparo oficial..." : L"Limpando cache...");
    HANDLE thread = CreateThread(NULL, 0, tool_worker, (LPVOID)(INT_PTR)tool, 0, NULL);
    if (thread) CloseHandle(thread);
    else { enable_tools(TRUE); set_status(L"Falha ao iniciar a operação"); }
}

static BOOL registry_key_exists(HKEY root, const wchar_t *path) {
    HKEY key;
    LONG result = RegOpenKeyExW(root, path, 0, KEY_READ, &key);
    if (result == ERROR_SUCCESS) RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static BOOL latest_log_path(wchar_t *output, size_t output_size) {
    wchar_t local[MAX_PATH], pattern[2 * MAX_PATH], candidate[2 * MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    swprintf(pattern, 2 * MAX_PATH, L"%ls\\Roblox\\logs\\*.log", local);
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    if (search == INVALID_HANDLE_VALUE) return FALSE;
    FILETIME latest = {0}; output[0] = 0;
    do {
        if (CompareFileTime(&data.ftLastWriteTime, &latest) > 0) {
            latest = data.ftLastWriteTime;
            swprintf(candidate, 2 * MAX_PATH, L"%ls\\Roblox\\logs\\%ls", local, data.cFileName);
            wcsncpy(output, candidate, output_size - 1); output[output_size - 1] = 0;
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return output[0] != 0;
}

static BOOL contains_ascii_ci(const char *text, const char *needle) {
    size_t needle_length = strlen(needle);
    if (!needle_length) return TRUE;
    for (; *text; ++text) {
        size_t i = 0;
        while (i < needle_length && text[i] &&
               tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i])) ++i;
        if (i == needle_length) return TRUE;
    }
    return FALSE;
}

static void analyze_latest_log(const wchar_t *path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return; }
    const DWORD capacity = 262144;
    DWORD amount = (DWORD)(size.QuadPart > capacity ? capacity : size.QuadPart);
    if (size.QuadPart > capacity) {
        LARGE_INTEGER offset; offset.QuadPart = size.QuadPart - capacity;
        SetFilePointerEx(file, offset, NULL, FILE_BEGIN);
    }
    char *data = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, amount + 1);
    if (!data) { CloseHandle(file); return; }
    DWORD read = 0;
    ReadFile(file, data, amount, &read, NULL);
    CloseHandle(file);
    data[read] = 0;
    if (contains_ascii_ci(data, "webview"))
        append_log(L"Log: referência a WebView detectada; Runtime ausente/corrompido é possível.");
    if (contains_ascii_ci(data, "d3d") || contains_ascii_ci(data, "graphics failed") ||
        contains_ascii_ci(data, "gpu crash"))
        append_log(L"Log: possível falha gráfica/Direct3D; atualize o driver da GPU.");
    if (contains_ascii_ci(data, "out of memory") || contains_ascii_ci(data, "bad allocation"))
        append_log(L"Log: possível falta de memória detectada.");
    if (contains_ascii_ci(data, "crash") || contains_ascii_ci(data, "exception"))
        append_log(L"Log: registros de crash/exceção encontrados; abra o log para detalhes.");
    HeapFree(GetProcessHeap(), 0, data);
}

static void diagnose_system(void) {
    append_log(L"--- Diagnóstico rápido ---");
    BOOL protocol = registry_key_exists(HKEY_CURRENT_USER,
        L"Software\\Classes\\roblox\\shell\\open\\command") ||
        registry_key_exists(HKEY_CLASSES_ROOT, L"roblox\\shell\\open\\command");
    append_log(protocol ? L"OK: protocolo roblox: registrado." : L"ERRO: protocolo roblox: ausente.");

    const wchar_t *wv64 = L"Software\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    const wchar_t *wv32 = L"Software\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    BOOL webview = registry_key_exists(HKEY_LOCAL_MACHINE, wv64) ||
        registry_key_exists(HKEY_CURRENT_USER, wv64) ||
        registry_key_exists(HKEY_LOCAL_MACHINE, wv32) ||
        registry_key_exists(HKEY_CURRENT_USER, wv32);
    append_log(webview ? L"OK: WebView2 Runtime detectado." : L"AVISO: WebView2 Runtime não detectado; telas brancas são prováveis.");

    wchar_t local[MAX_PATH], versions[2 * MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    swprintf(versions, 2 * MAX_PATH, L"%ls\\Roblox\\Versions", local);
    DWORD attributes = GetFileAttributesW(versions);
    append_log(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
        ? L"OK: diretório de versões do Roblox encontrado."
        : L"ERRO: instalação local do Roblox não foi encontrada.");

    MEMORYSTATUSEX memory = { sizeof(memory) };
    GlobalMemoryStatusEx(&memory);
    wchar_t message[256];
    swprintf(message, 256, L"Memória disponível: %llu MB de %llu MB.",
        memory.ullAvailPhys / (1024 * 1024), memory.ullTotalPhys / (1024 * 1024));
    append_log(message);
    append_log(roblox_is_running() ? L"RobloxPlayerBeta.exe está em execução." : L"Roblox não está em execução.");

    wchar_t log_path[2 * MAX_PATH];
    if (latest_log_path(log_path, 2 * MAX_PATH)) {
        append_log(L"Log recente encontrado; analisando os últimos 256 KB...");
        analyze_latest_log(log_path);
    } else {
        append_log(L"Nenhum log do Roblox foi encontrado.");
    }
    set_status(protocol ? L"Diagnóstico concluído" : L"Diagnóstico: instalação precisa de reparo");
}

static void open_latest_log(void) {
    wchar_t path[2 * MAX_PATH];
    if (!latest_log_path(path, 2 * MAX_PATH)) {
        MessageBoxW(g_window, L"Nenhum arquivo de log do Roblox foi encontrado.", APP_TITLE,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShellExecuteW(g_window, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL);
    append_log(L"Último log aberto no Bloco de Notas.");
}

static void launch_roblox(void) {
    wchar_t place[64], private_link[1024], code[160], type[64], uri[1400];
    GetWindowTextW(g_place, place, 64);
    GetWindowTextW(g_private, private_link, 1024);

    if (private_link[0]) {
        if (!official_https_url(private_link)) {
            MessageBoxW(g_window,
                L"Use somente um link HTTPS oficial de roblox.com.",
                APP_TITLE, MB_OK | MB_ICONWARNING);
            append_log(L"Link privado recusado: domínio não oficial.");
            return;
        }

        if (wcsstr(private_link, L"/share?") || wcsstr(private_link, L"/share/?")) {
            if (!query_value(private_link, L"code", code, 160) || !valid_code(code) ||
                !query_value(private_link, L"type", type, 64) || _wcsicmp(type, L"Server") != 0) {
                MessageBoxW(g_window, L"O link compartilhado está incompleto ou inválido.",
                            APP_TITLE, MB_OK | MB_ICONWARNING);
                append_log(L"Código de compartilhamento inválido.");
                return;
            }
            swprintf(uri, 1400, L"roblox://navigation/share_links?code=%ls&type=Server", code);
            append_log(L"Solicitando servidor privado pelo protocolo oficial.");
        } else {
            if (!query_value(private_link, L"privateServerLinkCode", code, 160) || !valid_code(code)) {
                MessageBoxW(g_window,
                    L"O link não contém um código de servidor privado reconhecido.",
                    APP_TITLE, MB_OK | MB_ICONWARNING);
                append_log(L"Link privado legado inválido.");
                return;
            }
            if (!valid_place_id(place) && !place_from_game_url(private_link, place, 64)) {
                MessageBoxW(g_window, L"Informe um Place ID válido.",
                            APP_TITLE, MB_OK | MB_ICONWARNING);
                return;
            }
            swprintf(uri, 1400, L"roblox://placeId=%ls&linkCode=%ls", place, code);
            append_log(L"Solicitando experiência em servidor privado.");
        }
    } else {
        if (!valid_place_id(place)) {
            MessageBoxW(g_window,
                L"Informe um Place ID contendo somente números (até 20 dígitos).",
                APP_TITLE, MB_OK | MB_ICONWARNING);
            append_log(L"Place ID inválido.");
            return;
        }
        swprintf(uri, 1400, L"roblox://placeId=%ls", place);
        append_log(L"Solicitando experiência pública.");
    }

    set_status(L"Iniciando Roblox...");
    if (!dispatch_uri(uri)) {
        set_status(L"Roblox Player não encontrado");
        append_log(L"O protocolo roblox: não está registrado.");
        MessageBoxW(g_window,
            L"O Windows não encontrou o protocolo oficial do Roblox. Repare ou reinstale o Roblox Player.",
            APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }

    wcsncpy(g_last_uri, uri, 1399);
    g_last_uri[1399] = 0;
    g_seen_roblox = FALSE;
    g_restart_count = 0;
    g_tracking = SendMessageW(g_keep_open, BM_GETCHECK, 0, 0) == BST_CHECKED;
    set_status(g_tracking ? L"Comando enviado; monitoramento ativo" : L"Comando enviado ao Roblox");
    append_log(g_tracking
        ? L"Protocolo entregue. Reabertura opcional ativada; desmarque para parar."
        : L"Protocolo entregue ao Windows. Fire-and-forget concluído.");
}

static void draw_action_button(DRAWITEMSTRUCT *draw) {
    int id = GetDlgCtrlID(draw->hwndItem);
    COLORREF base = id == ID_REPAIR ? RGB(22, 163, 74) :
                    id == ID_LAUNCH ? g_primary : RGB(55, 65, 81);
    COLORREF color = (draw->itemState & ODS_SELECTED)
        ? RGB(GetRValue(base) * 4 / 5, GetGValue(base) * 4 / 5, GetBValue(base) * 4 / 5)
        : base;
    if (draw->itemState & ODS_DISABLED) color = RGB(70, 78, 92);
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(draw->hDC, &draw->rcItem, brush);
    DeleteObject(brush);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, RGB(255, 255, 255));
    SelectObject(draw->hDC, g_bold);
    wchar_t text[64];
    GetWindowTextW(draw->hwndItem, text, 64);
    DrawTextW(draw->hDC, text, -1, &draw->rcItem,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (draw->itemState & ODS_FOCUS) {
        RECT focus = draw->rcItem;
        InflateRect(&focus, -5, -5);
        DrawFocusRect(draw->hDC, &focus);
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_LAUNCH) {
            launch_roblox();
        } else if (LOWORD(wparam) == ID_DIAGNOSE) {
            diagnose_system();
        } else if (LOWORD(wparam) == ID_REPAIR) {
            if (MessageBoxW(hwnd,
                L"O reparo fechará o Roblox, validará um instalador oficial e substituirá arquivos locais corrompidos. Sua conta e seu progresso online não serão apagados. Continuar?",
                APP_TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                start_tool(TOOL_REPAIR);
        } else if (LOWORD(wparam) == ID_CLEAR_CACHE) {
            start_tool(TOOL_CACHE);
        } else if (LOWORD(wparam) == ID_OPEN_LOG) {
            open_latest_log();
        } else if (LOWORD(wparam) == ID_KEEP_OPEN && HIWORD(wparam) == BN_CLICKED) {
            BOOL enabled = SendMessageW(g_keep_open, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!enabled) {
                g_tracking = FALSE;
                g_seen_roblox = FALSE;
                append_log(L"Reabertura automática desativada pelo usuário.");
            } else {
                append_log(L"Reabertura automática ativada. Fechar o launcher sempre a desativa.");
                if (g_last_uri[0]) {
                    g_tracking = TRUE;
                    g_seen_roblox = roblox_is_running();
                    g_last_launch_time = GetTickCount64();
                    g_restart_count = 0;
                }
            }
        }
        return 0;
    case WM_TOOL_STATUS:
        set_status((const wchar_t*)lparam);
        append_log((const wchar_t*)lparam);
        return 0;
    case WM_TOOL_DONE: {
        enable_tools(TRUE);
        DWORD result = (DWORD)wparam;
        if (result == 0) {
            if (lparam == TOOL_REPAIR) {
                set_status(L"Roblox reparado e atualizado");
                append_log(L"SUCESSO: instalação oficial concluída.");
                MessageBoxW(hwnd, L"O Roblox foi reparado com o instalador oficial.", APP_TITLE,
                            MB_OK | MB_ICONINFORMATION);
            } else {
                set_status(L"Cache limpo com sucesso");
                append_log(L"SUCESSO: caches e temporários foram limpos.");
            }
        } else {
            set_status(L"A operação encontrou um erro");
            const wchar_t *error = result == ERR_DOWNLOAD ?
                L"Falha ao baixar dados oficiais. Verifique internet, data/hora e firewall." :
                result == ERR_SIGNATURE ?
                L"O instalador não passou na assinatura digital e foi apagado." :
                result == ERR_INSTALLER ?
                L"O instalador oficial não concluiu. Reinicie o Windows e tente novamente." :
                L"Alguns arquivos estavam em uso. Feche o Roblox e tente novamente.";
            append_log(error);
            MessageBoxW(hwnd, error, APP_TITLE, MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_TIMER:
        if (wparam == WATCHDOG_TIMER) monitor_roblox();
        return 0;
    case WM_DRAWITEM:
        if ((UINT)wparam == ID_LAUNCH || (UINT)wparam == ID_DIAGNOSE ||
            (UINT)wparam == ID_REPAIR || (UINT)wparam == ID_CLEAR_CACHE ||
            (UINT)wparam == ID_OPEN_LOG) {
            draw_action_button((DRAWITEMSTRUCT*)lparam);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wparam;
        SetTextColor(dc, g_text);
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_background_brush;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wparam;
        SetTextColor(dc, g_text);
        SetBkColor(dc, g_surface);
        return (LRESULT)g_input_brush;
    }
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wparam;
        SetTextColor(dc, g_text);
        SetBkColor(dc, g_background);
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_background_brush;
    }
    case WM_ERASEBKGND: {
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect((HDC)wparam, &rect, g_background_brush);
        return 1;
    }
    case WM_DESTROY:
        KillTimer(hwnd, WATCHDOG_TIMER);
        g_tracking = FALSE;
        DeleteObject(g_font); DeleteObject(g_bold); DeleteObject(g_title);
        DeleteObject(g_background_brush); DeleteObject(g_input_brush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)command_line;
    BOOL dark = TRUE; /* O Bootstrapper Advanced usa tema escuro fixo. */
    g_background = RGB(17, 19, 24);
    g_surface = dark ? RGB(29, 33, 41) : RGB(226, 231, 239);
    g_text = dark ? RGB(241, 245, 249) : RGB(24, 28, 36);
    g_muted = dark ? RGB(156, 163, 175) : RGB(90, 96, 108);
    g_primary = RGB(37, 99, 235);
    g_background_brush = CreateSolidBrush(g_background);
    g_input_brush = CreateSolidBrush(g_surface);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = g_background_brush;
    wc.lpszClassName = L"RobloxBootstrapperAdvancedWindow";
    RegisterClassExW(&wc);

    RECT rect = {0, 0, 680, 680};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    g_window = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, style,
        x, y, width, height, NULL, NULL, instance, NULL);
    if (!g_window) return 1;
    DwmSetWindowAttribute(g_window, 20, &dark, sizeof(dark));

    g_font = CreateFontW(19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_bold = CreateFontW(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_title = CreateFontW(32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    create_label(APP_TITLE, 35, 18, 610, 44, SS_CENTER, g_title);
    create_label(L"Launcher, diagnóstico e reparo oficial em uma interface nativa", 35, 62, 610, 26, SS_CENTER, g_font);
    create_label(L"ID da Experiência (Place ID)", 50, 101, 580, 26, SS_LEFT, g_bold);
    g_place = create_input(L"Exemplo: 1818", ID_PLACE, 50, 130, 580, 41);
    create_label(L"Link de servidor privado (opcional)", 50, 183, 580, 26, SS_LEFT, g_bold);
    g_private = create_input(L"https://www.roblox.com/share?code=...&type=Server",
                             ID_PRIVATE, 50, 212, 580, 41);
    HWND warning = create_label(L"Segurança: nunca cole .ROBLOSECURITY, cookies ou senhas.",
                                50, 260, 580, 24, SS_CENTER, g_font);
    (void)warning;

    g_keep_open = CreateWindowExW(0, L"BUTTON",
        L"Reabrir se o Roblox fechar (opcional; máximo de 3 tentativas)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        50, 287, 580, 25, g_window, (HMENU)ID_KEEP_OPEN, instance, NULL);
    set_font(g_keep_open, g_font);
    SendMessageW(g_keep_open, BM_SETCHECK, BST_UNCHECKED, 0);

    g_launch = CreateWindowExW(0, L"BUTTON", L"Iniciar Roblox",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        50, 321, 580, 52, g_window, (HMENU)ID_LAUNCH, instance, NULL);
    set_font(g_launch, g_bold);
    rounded_region(g_launch, 580, 52, 14);

    create_label(L"Ferramentas de recuperação", 50, 389, 580, 26, SS_LEFT, g_bold);
    g_diagnose = CreateWindowExW(0, L"BUTTON", L"Diagnosticar Sistema",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        50, 420, 280, 45, g_window, (HMENU)ID_DIAGNOSE, instance, NULL);
    g_repair = CreateWindowExW(0, L"BUTTON", L"Reparar / Atualizar Oficial",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        350, 420, 280, 45, g_window, (HMENU)ID_REPAIR, instance, NULL);
    g_cache = CreateWindowExW(0, L"BUTTON", L"Limpar Cache",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        50, 477, 280, 45, g_window, (HMENU)ID_CLEAR_CACHE, instance, NULL);
    g_open_log = CreateWindowExW(0, L"BUTTON", L"Abrir Último Log",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        350, 477, 280, 45, g_window, (HMENU)ID_OPEN_LOG, instance, NULL);
    HWND tool_buttons[] = { g_diagnose, g_repair, g_cache, g_open_log };
    for (int i = 0; i < 4; ++i) {
        set_font(tool_buttons[i], g_bold);
        rounded_region(tool_buttons[i], 280, 45, 12);
    }

    g_status = create_label(L"Pronto", 50, 540, 580, 24, SS_LEFT, g_bold);
    g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"[--:--:--] Pronto. Interface nativa sem WebView.\r\n",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        50, 570, 580, 80, g_window, NULL, instance, NULL);
    set_font(g_log, g_font);
    SetTimer(g_window, WATCHDOG_TIMER, 2000, NULL);

    ShowWindow(g_window, show);
    UpdateWindow(g_window);
    SetFocus(g_place);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
