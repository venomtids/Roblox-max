#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <wininet.h>
#include <wintrust.h>
#include <softpub.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define APP_NAME L"Roblox Fixer & Launcher Pro"
#define WM_TASK_DONE (WM_APP + 1)
#define WM_TASK_STATUS (WM_APP + 2)
#define ID_TAB_FIXES 100
#define ID_TAB_LAUNCH 101
#define ID_CACHE 200
#define ID_GRAPHICS 201
#define ID_NETWORK 202
#define ID_REPAIR 203
#define ID_PLACE 300
#define ID_LAUNCH 301
#define ERR_DOWNLOAD 0xE001
#define ERR_SIGNATURE 0xE002
#define ERR_INSTALLER 0xE003

static HWND g_window, g_status, g_progress, g_place;
static HWND g_fix_controls[6], g_launch_controls[3];
static HWND g_buttons[7];
static HFONT g_font, g_font_bold, g_font_title;
static HBRUSH g_bg_brush, g_edit_brush;
static COLORREF g_bg, g_panel, g_text, g_muted, g_accent;
static BOOL g_dark = TRUE, g_busy = FALSE;

static BOOL system_dark_mode(void) {
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

static void set_control_font(HWND h, HFONT font) {
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
}

static HWND make_static(const wchar_t *text, int x, int y, int w, int h, DWORD style) {
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, g_window, NULL, GetModuleHandleW(NULL), NULL);
    set_control_font(control, g_font);
    return control;
}

static HWND make_button(const wchar_t *text, int id, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        x, y, w, h, g_window, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, 14, 14);
    SetWindowRgn(control, region, TRUE);
    set_control_font(control, g_font_bold);
    return control;
}

static void show_section(BOOL fixes) {
    for (int i = 0; i < 6; ++i) ShowWindow(g_fix_controls[i], fixes ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < 3; ++i) ShowWindow(g_launch_controls[i], fixes ? SW_HIDE : SW_SHOW);
    SetWindowLongPtrW(g_window, GWLP_USERDATA, fixes ? 0 : 1);
    InvalidateRect(g_buttons[0], NULL, TRUE);
    InvalidateRect(g_buttons[1], NULL, TRUE);
}

static void set_busy(BOOL busy) {
    g_busy = busy;
    EnableWindow(g_buttons[2], !busy);
    EnableWindow(g_buttons[3], !busy);
    EnableWindow(g_buttons[4], !busy);
    EnableWindow(g_buttons[5], !busy);
    SendMessageW(g_progress, PBM_SETMARQUEE, busy, 25);
    SetWindowTextW(g_status, busy ? L"Corrigindo..." : L"Pronto");
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

static DWORD download_and_run_official_installer(void) {
    static const wchar_t *metadata_url =
        L"https://clientsettingscdn.roblox.com/v2/client-version/WindowsPlayer/channel/LIVE";
    wchar_t temp[MAX_PATH], metadata[2 * MAX_PATH], installer[2 * MAX_PATH];
    wchar_t installer_url[512], version_wide[96];
    char json[8192] = {0}, version[96] = {0};
    if (!GetTempPathW(MAX_PATH, temp)) return ERR_DOWNLOAD;
    swprintf(metadata, 2 * MAX_PATH, L"%lsRoblox-versao-oficial.json", temp);
    swprintf(installer, 2 * MAX_PATH, L"%lsRobloxPlayerInstaller-oficial.exe", temp);
    DeleteFileW(metadata); DeleteFileW(installer);
    DeleteUrlCacheEntryW(metadata_url);
    PostMessageW(g_window, WM_TASK_STATUS, 0, (LPARAM)L"Consultando a versão oficial...");

    if (FAILED(URLDownloadToFileW(NULL, metadata_url, metadata, 0, NULL)))
        return ERR_DOWNLOAD;
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
    PostMessageW(g_window, WM_TASK_STATUS, 0, (LPARAM)L"Baixando o instalador oficial...");

    if (FAILED(URLDownloadToFileW(NULL, installer_url, installer, 0, NULL)))
        return ERR_DOWNLOAD;
    file = CreateFileW(installer, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BYTE header[2] = {0}; read = 0;
    if (file != INVALID_HANDLE_VALUE) {
        ReadFile(file, header, 2, &read, NULL); CloseHandle(file);
    }
    PostMessageW(g_window, WM_TASK_STATUS, 0, (LPARAM)L"Verificando a assinatura digital...");
    if (read != 2 || header[0] != 'M' || header[1] != 'Z' ||
        !verify_authenticode(installer)) {
        DeleteFileW(installer);
        return ERR_SIGNATURE;
    }

    /* Só remove a instalação antiga depois de obter um instalador válido. */
    PostMessageW(g_window, WM_TASK_STATUS, 0, (LPARAM)L"Removendo arquivos locais corrompidos...");
    wchar_t local[MAX_PATH], roblox_temp[MAX_PATH], path[2 * MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    GetEnvironmentVariableW(L"TEMP", roblox_temp, MAX_PATH);
    stop_roblox_processes();
    swprintf(path, 2 * MAX_PATH, L"%ls\\Roblox", local);
    clear_directory(path); RemoveDirectoryW(path);
    swprintf(path, 2 * MAX_PATH, L"%ls\\Roblox", roblox_temp);
    clear_directory(path); RemoveDirectoryW(path);

    PostMessageW(g_window, WM_TASK_STATUS, 0, (LPARAM)L"Instalando a versão mais recente...");
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = g_window;
    info.lpVerb = L"open";
    info.lpFile = installer;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        DeleteFileW(installer);
        return ERR_INSTALLER;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
    DeleteFileW(installer);
    return exit_code == 0 ? 0 : ERR_INSTALLER;
}

static DWORD run_network_elevated(void) {
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = g_window;
    info.lpVerb = L"runas";
    info.lpFile = L"cmd.exe";
    info.lpParameters = L"/d /s /c \"ipconfig /flushdns >nul 2>&1 && netsh winsock reset >nul 2>&1\"";
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) return GetLastError();
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code;
}

static DWORD WINAPI repair_worker(LPVOID parameter) {
    int action = (int)(INT_PTR)parameter;
    DWORD result = 1;
    wchar_t local[MAX_PATH], temp[MAX_PATH], path[2 * MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    GetEnvironmentVariableW(L"TEMP", temp, MAX_PATH);

    if (action == ID_CACHE) {
        const wchar_t *suffixes[] = {
            L"Roblox\\Downloads", L"Roblox\\logs", L"Roblox\\http",
            L"Roblox\\LocalStorage\\roblox"
        };
        result = 0;
        for (int i = 0; i < 4; ++i) {
            swprintf(path, 2 * MAX_PATH, L"%ls\\%ls", local, suffixes[i]);
            if (!clear_directory(path)) result = 1;
        }
        swprintf(path, 2 * MAX_PATH, L"%ls\\Roblox", temp);
        if (!clear_directory(path)) result = 1;
    } else if (action == ID_GRAPHICS) {
        swprintf(path, 2 * MAX_PATH, L"%ls\\Roblox\\GlobalBasicSettings_13.xml", local);
        result = (DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND) ? 0 : 1;
    } else if (action == ID_NETWORK) {
        result = run_network_elevated();
    } else if (action == ID_REPAIR) {
        result = download_and_run_official_installer();
    }
    PostMessageW(g_window, WM_TASK_DONE, (WPARAM)result, (LPARAM)action);
    return 0;
}

static void start_repair(int action) {
    if (g_busy) return;
    set_busy(TRUE);
    HANDLE thread = CreateThread(NULL, 0, repair_worker, (LPVOID)(INT_PTR)action, 0, NULL);
    if (thread) CloseHandle(thread);
    else {
        set_busy(FALSE);
        MessageBoxW(g_window, L"Não foi possível iniciar a correção.", APP_NAME, MB_ICONERROR);
    }
}

static void launch_game(void) {
    wchar_t id[32], uri[128];
    GetWindowTextW(g_place, id, 32);
    size_t length = wcslen(id);
    if (length < 1 || length > 20) {
        MessageBoxW(g_window, L"Digite um ID válido contendo apenas números.", APP_NAME, MB_ICONWARNING);
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        if (id[i] < L'0' || id[i] > L'9') {
            MessageBoxW(g_window, L"O ID pode conter somente números.", APP_NAME, MB_ICONWARNING);
            return;
        }
    }
    /* Deeplink universal oficial: abre o Player diretamente, sem Edge. */
    swprintf(uri, 128, L"roblox://experiences/start?placeId=%ls", id);
    HINSTANCE result = ShellExecuteW(g_window, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        MessageBoxW(g_window, L"Não foi possível abrir o Roblox Player. Confirme se ele está instalado.", APP_NAME, MB_ICONERROR);
    } else SetWindowTextW(g_status, L"Roblox iniciado!");
}

static void draw_button(DRAWITEMSTRUCT *draw) {
    wchar_t text[128];
    GetWindowTextW(draw->hwndItem, text, 128);
    int id = GetDlgCtrlID(draw->hwndItem);
    BOOL selected = ((GetWindowLongPtrW(g_window, GWLP_USERDATA) == 0 && id == ID_TAB_FIXES) ||
                     (GetWindowLongPtrW(g_window, GWLP_USERDATA) == 1 && id == ID_TAB_LAUNCH));
    BOOL pressed = (draw->itemState & ODS_SELECTED) != 0;
    BOOL disabled = (draw->itemState & ODS_DISABLED) != 0;
    COLORREF color = (id == ID_LAUNCH) ? RGB(22, 163, 74) :
                     ((selected || (id >= ID_CACHE && id <= ID_REPAIR)) ? g_accent : g_panel);
    if (pressed) color = RGB(GetRValue(color) * 4 / 5, GetGValue(color) * 4 / 5, GetBValue(color) * 4 / 5);
    if (disabled) color = g_panel;
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(draw->hDC, &draw->rcItem, brush);
    DeleteObject(brush);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, disabled ? g_muted : RGB(255,255,255));
    SelectObject(draw->hDC, g_font_bold);
    DrawTextW(draw->hDC, text, -1, &draw->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (draw->itemState & ODS_FOCUS) {
        RECT focus = draw->rcItem; InflateRect(&focus, -4, -4); DrawFocusRect(draw->hDC, &focus);
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        int id = LOWORD(wparam);
        if (id == ID_TAB_FIXES) show_section(TRUE);
        else if (id == ID_TAB_LAUNCH) show_section(FALSE);
        else if (id >= ID_CACHE && id <= ID_NETWORK) start_repair(id);
        else if (id == ID_REPAIR) {
            int answer = MessageBoxW(hwnd,
                L"Esta correção fechará o Roblox, removerá os arquivos locais corrompidos e instalará a versão oficial mais recente. Sua conta e seu progresso online não serão apagados, mas talvez seja necessário entrar novamente.\n\nContinuar?",
                APP_NAME, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer == IDYES) start_repair(id);
        } else if (id == ID_LAUNCH) launch_game();
        return 0;
    }
    case WM_TASK_STATUS:
        SetWindowTextW(g_status, (const wchar_t*)lparam);
        return 0;
    case WM_TASK_DONE: {
        set_busy(FALSE);
        if ((DWORD)wparam == 0) {
            SetWindowTextW(g_status, L"Sucesso!");
            const wchar_t *message = (lparam == ID_CACHE) ? L"Cache e temporários foram limpos." :
                (lparam == ID_GRAPHICS) ? L"As configurações gráficas foram redefinidas." :
                (lparam == ID_NETWORK) ? L"DNS e Winsock redefinidos. Reinicie o computador para concluir." :
                L"O Roblox foi reparado e atualizado com o instalador oficial. Tente abri-lo novamente.";
            MessageBoxW(hwnd, message, APP_NAME, MB_OK | MB_ICONINFORMATION);
        } else {
            SetWindowTextW(g_status, L"Falha na correção");
            const wchar_t *message = ((DWORD)wparam == ERROR_CANCELLED) ?
                L"A permissão de administrador foi cancelada." :
                ((DWORD)wparam == ERR_DOWNLOAD) ? L"Não foi possível baixar o instalador oficial. Verifique a internet, data/hora, firewall e tente novamente." :
                ((DWORD)wparam == ERR_SIGNATURE) ? L"O instalador baixado não passou na verificação de assinatura digital e foi apagado por segurança." :
                ((DWORD)wparam == ERR_INSTALLER) ? L"O instalador oficial não conseguiu concluir. Reinicie o Windows e tente novamente." :
                L"A operação não foi concluída. Feche o Roblox e tente novamente.";
            MessageBoxW(hwnd, message, APP_NAME, MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_DRAWITEM:
        draw_button((DRAWITEMSTRUCT*)lparam); return TRUE;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wparam;
        SetTextColor(dc, g_text); SetBkColor(dc, g_bg); SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_bg_brush;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wparam;
        SetTextColor(dc, g_text); SetBkColor(dc, g_panel);
        return (LRESULT)g_edit_brush;
    }
    case WM_ERASEBKGND: {
        RECT rect; GetClientRect(hwnd, &rect); FillRect((HDC)wparam, &rect, g_bg_brush); return 1;
    }
    case WM_DESTROY:
        DeleteObject(g_font); DeleteObject(g_font_bold); DeleteObject(g_font_title);
        DeleteObject(g_bg_brush); DeleteObject(g_edit_brush);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)command_line;
    g_dark = system_dark_mode();
    g_bg = g_dark ? RGB(18,18,22) : RGB(244,246,249);
    g_panel = g_dark ? RGB(43,43,50) : RGB(224,228,235);
    g_text = g_dark ? RGB(240,240,245) : RGB(24,28,36);
    g_muted = g_dark ? RGB(160,163,174) : RGB(95,100,110);
    g_accent = RGB(36, 99, 235);
    g_bg_brush = CreateSolidBrush(g_bg); g_edit_brush = CreateSolidBrush(g_panel);

    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&controls);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = window_proc; wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = g_bg_brush; wc.lpszClassName = L"RobloxFixerNative";
    RegisterClassExW(&wc);

    RECT rect = {0,0,600,580};
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    g_window = CreateWindowExW(0, wc.lpszClassName, APP_NAME,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, width, height, NULL, NULL, instance, NULL);
    BOOL dark = g_dark; DwmSetWindowAttribute(g_window, 20, &dark, sizeof(dark));

    g_font = CreateFontW(18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_font_bold = CreateFontW(18,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_font_title = CreateFontW(29,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");

    HWND title = make_static(APP_NAME, 30, 20, 540, 38, SS_CENTER); set_control_font(title, g_font_title);
    HWND subtitle = make_static(L"Reparo seguro, atualização oficial e acesso direto", 30, 58, 540, 24, SS_CENTER);
    g_buttons[0] = make_button(L"Correções", ID_TAB_FIXES, 30, 94, 265, 40);
    g_buttons[1] = make_button(L"Launcher Direto", ID_TAB_LAUNCH, 305, 94, 265, 40);

    g_buttons[2] = g_fix_controls[0] = make_button(L"Limpar Cache e Temporários", ID_CACHE, 50, 153, 500, 50);
    g_buttons[3] = g_fix_controls[1] = make_button(L"Redefinir Configurações Gráficas (XML)", ID_GRAPHICS, 50, 214, 500, 50);
    g_buttons[4] = g_fix_controls[2] = make_button(L"Otimizar Rede (DNS/Winsock)", ID_NETWORK, 50, 275, 500, 50);
    g_buttons[5] = g_fix_controls[3] = make_button(L"Reparar Tela Branca / Atualizar Roblox", ID_REPAIR, 50, 336, 500, 54);
    g_fix_controls[4] = make_static(L"Usa somente o instalador oficial e verifica a assinatura digital.", 50, 402, 500, 22, SS_CENTER);
    g_fix_controls[5] = make_static(L"O Roblox será fechado durante o reparo completo.", 50, 427, 500, 22, SS_CENTER);

    g_launch_controls[0] = make_static(L"Abra uma experiência diretamente no Roblox Player, sem Edge", 50, 190, 500, 28, SS_CENTER);
    g_place = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_CENTER | ES_AUTOHSCROLL,
        80, 247, 440, 48, g_window, (HMENU)ID_PLACE, instance, NULL);
    set_control_font(g_place, g_font); SendMessageW(g_place, EM_SETCUEBANNER, TRUE, (LPARAM)L"Cole o ID do Jogo/Mapa aqui");
    g_launch_controls[1] = g_place;
    g_buttons[6] = g_launch_controls[2] = make_button(L"Iniciar Jogo (Sem Navegador)", ID_LAUNCH, 80, 318, 440, 58);

    g_status = make_static(L"Pronto", 36, 500, 528, 24, SS_LEFT);
    g_progress = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
        36, 532, 528, 7, g_window, NULL, instance, NULL);
    SendMessageW(g_progress, PBM_SETSTATE, PBST_NORMAL, 0);
    show_section(TRUE);
    ShowWindow(g_window, show); UpdateWindow(g_window);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return (int)msg.wParam;
}
