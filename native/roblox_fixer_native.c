#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <wchar.h>

#define APP_NAME L"Roblox Fixer & Launcher Pro"
#define WM_TASK_DONE (WM_APP + 1)
#define ID_TAB_FIXES 100
#define ID_TAB_LAUNCH 101
#define ID_CACHE 200
#define ID_GRAPHICS 201
#define ID_NETWORK 202
#define ID_PLACE 300
#define ID_LAUNCH 301

static HWND g_window, g_status, g_progress, g_place;
static HWND g_fix_controls[5], g_launch_controls[3];
static HWND g_buttons[6];
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
    for (int i = 0; i < 5; ++i) ShowWindow(g_fix_controls[i], fixes ? SW_SHOW : SW_HIDE);
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
    swprintf(uri, 128, L"roblox-player:1+launchmode:play+gameinfo:%ls", id);
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
                     ((selected || (id >= ID_CACHE && id <= ID_NETWORK)) ? g_accent : g_panel);
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
        else if (id == ID_LAUNCH) launch_game();
        return 0;
    }
    case WM_TASK_DONE: {
        set_busy(FALSE);
        if ((DWORD)wparam == 0) {
            SetWindowTextW(g_status, L"Sucesso!");
            const wchar_t *message = (lparam == ID_CACHE) ? L"Cache e temporários foram limpos." :
                (lparam == ID_GRAPHICS) ? L"As configurações gráficas foram redefinidas." :
                L"DNS e Winsock redefinidos. Reinicie o computador para concluir.";
            MessageBoxW(hwnd, message, APP_NAME, MB_OK | MB_ICONINFORMATION);
        } else {
            SetWindowTextW(g_status, L"Falha na correção");
            const wchar_t *message = ((DWORD)wparam == ERROR_CANCELLED) ?
                L"A permissão de administrador foi cancelada." :
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

    RECT rect = {0,0,540,500};
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

    HWND title = make_static(APP_NAME, 28, 20, 484, 38, SS_CENTER); set_control_font(title, g_font_title);
    HWND subtitle = make_static(L"Correções rápidas e acesso direto ao Roblox", 28, 58, 484, 24, SS_CENTER);
    g_buttons[0] = make_button(L"Correções", ID_TAB_FIXES, 28, 94, 238, 40);
    g_buttons[1] = make_button(L"Launcher Direto", ID_TAB_LAUNCH, 274, 94, 238, 40);

    g_buttons[2] = g_fix_controls[0] = make_button(L"Limpar Cache e Temporários", ID_CACHE, 46, 158, 448, 56);
    g_buttons[3] = g_fix_controls[1] = make_button(L"Redefinir Configurações Gráficas (XML)", ID_GRAPHICS, 46, 227, 448, 56);
    g_buttons[4] = g_fix_controls[2] = make_button(L"Otimizar Rede (DNS/Winsock)", ID_NETWORK, 46, 296, 448, 56);
    g_fix_controls[3] = make_static(L"A otimização de rede solicitará permissão de administrador.", 46, 363, 448, 24, SS_CENTER);
    g_fix_controls[4] = make_static(L"", 0, 0, 1, 1, 0);

    g_launch_controls[0] = make_static(L"Abra uma experiência sem passar pelo navegador", 46, 174, 448, 28, SS_CENTER);
    g_place = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_CENTER | ES_AUTOHSCROLL,
        70, 224, 400, 48, g_window, (HMENU)ID_PLACE, instance, NULL);
    set_control_font(g_place, g_font); SendMessageW(g_place, EM_SETCUEBANNER, TRUE, (LPARAM)L"Cole o ID do Jogo/Mapa aqui");
    g_launch_controls[1] = g_place;
    g_buttons[5] = g_launch_controls[2] = make_button(L"Iniciar Jogo (Ignorar Navegador)", ID_LAUNCH, 70, 294, 400, 58);

    g_status = make_static(L"Pronto", 32, 420, 476, 24, SS_LEFT);
    g_progress = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
        32, 452, 476, 7, g_window, NULL, instance, NULL);
    SendMessageW(g_progress, PBM_SETSTATE, PBST_NORMAL, 0);
    show_section(TRUE);
    ShowWindow(g_window, show); UpdateWindow(g_window);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return (int)msg.wParam;
}
