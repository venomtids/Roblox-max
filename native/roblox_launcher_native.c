#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wchar.h>
#include <wctype.h>

#define APP_TITLE L"Roblox Native Launcher"
#define ID_PLACE 101
#define ID_PRIVATE 102
#define ID_LAUNCH 103

static HWND g_window, g_place, g_private, g_status, g_log, g_launch;
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
    wchar_t old_text[1024], new_text[1400];
    GetWindowTextW(g_log, old_text, 1024);
    swprintf(new_text, 1400, L"%ls[%02u:%02u:%02u] %ls\r\n",
             old_text, time.wHour, time.wMinute, time.wSecond, message);
    SetWindowTextW(g_log, new_text);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void set_status(const wchar_t *text) {
    SetWindowTextW(g_status, text);
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
    HINSTANCE result = ShellExecuteW(g_window, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        set_status(L"Roblox Player não encontrado");
        append_log(L"O protocolo roblox: não está registrado.");
        MessageBoxW(g_window,
            L"O Windows não encontrou o protocolo oficial do Roblox. Repare ou reinstale o Roblox Player.",
            APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }
    set_status(L"Comando enviado ao Roblox");
    append_log(L"Protocolo entregue ao Windows. Fire-and-forget concluído.");
}

static void draw_launch_button(DRAWITEMSTRUCT *draw) {
    COLORREF color = (draw->itemState & ODS_SELECTED) ? RGB(29, 78, 216) : g_primary;
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
        if (LOWORD(wparam) == ID_LAUNCH) launch_roblox();
        return 0;
    case WM_DRAWITEM:
        if ((UINT)wparam == ID_LAUNCH) {
            draw_launch_button((DRAWITEMSTRUCT*)lparam);
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
    case WM_ERASEBKGND: {
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect((HDC)wparam, &rect, g_background_brush);
        return 1;
    }
    case WM_DESTROY:
        DeleteObject(g_font); DeleteObject(g_bold); DeleteObject(g_title);
        DeleteObject(g_background_brush); DeleteObject(g_input_brush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)command_line;
    BOOL dark = system_uses_dark_mode();
    g_background = dark ? RGB(17, 19, 24) : RGB(244, 246, 249);
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
    wc.lpszClassName = L"RobloxNativeLauncherWindow";
    RegisterClassExW(&wc);

    RECT rect = {0, 0, 600, 530};
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

    create_label(APP_TITLE, 35, 20, 530, 44, SS_CENTER, g_title);
    create_label(L"Inicialização direta, nativa e sem navegador", 35, 65, 530, 26, SS_CENTER, g_font);
    create_label(L"ID da Experiência (Place ID)", 50, 112, 500, 26, SS_LEFT, g_bold);
    g_place = create_input(L"Exemplo: 1818", ID_PLACE, 50, 141, 500, 43);
    create_label(L"Link de servidor privado (opcional)", 50, 205, 500, 26, SS_LEFT, g_bold);
    g_private = create_input(L"https://www.roblox.com/share?code=...&type=Server",
                             ID_PRIVATE, 50, 234, 500, 43);
    HWND warning = create_label(L"Segurança: nunca cole .ROBLOSECURITY, cookies ou senhas.",
                                50, 292, 500, 28, SS_CENTER, g_font);
    (void)warning;

    g_launch = CreateWindowExW(0, L"BUTTON", L"Iniciar Roblox",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        50, 337, 500, 58, g_window, (HMENU)ID_LAUNCH, instance, NULL);
    set_font(g_launch, g_bold);
    rounded_region(g_launch, 500, 58, 14);

    g_status = create_label(L"Pronto", 50, 411, 500, 24, SS_LEFT, g_bold);
    g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"[--:--:--] Pronto. Nenhum WebView ou navegador foi carregado.\r\n",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        50, 441, 500, 59, g_window, NULL, instance, NULL);
    set_font(g_log, g_font);

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
