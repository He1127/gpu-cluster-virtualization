/* Read-only Win32 viewer for actual experiment log files. No simulated output. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define VIEWER_MAX_PATH 32768
#define VIEWER_MAX_BYTES (8u * 1024u * 1024u)

typedef struct LogPane {
    HWND label;
    HWND edit;
    wchar_t path[VIEWER_MAX_PATH];
    FILETIME last_write;
    LARGE_INTEGER last_size;
    int loaded;
    int missing;
} LogPane;

static LogPane panes[4];
static int pane_count = 0;
static HWND heading;
static HFONT log_font;
static HFONT label_font;
static HBRUSH white_brush;
static wchar_t source_description[VIEWER_MAX_PATH + 128];
static wchar_t window_title[1024] = L"Socket\x5b9e\x9a8c\x4e09\x8fd0\x884c\x8bb0\x5f55";

static int combine_path(wchar_t *out, size_t capacity, const wchar_t *directory,
                        const wchar_t *filename)
{
    const size_t length = wcslen(directory);
    int written = _snwprintf_s(out, capacity, _TRUNCATE, L"%s%s%s", directory,
                              length && directory[length - 1] != L'\\' &&
                              directory[length - 1] != L'/' ? L"\\" : L"", filename);
    return written >= 0;
}

static void show_waiting(LogPane *pane, DWORD error)
{
    wchar_t *message;
    size_t capacity;
    if (pane->missing) return;
    capacity = wcslen(pane->path) + 180;
    message = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  capacity * sizeof(wchar_t));
    if (message) {
        _snwprintf_s(message, capacity, _TRUNCATE,
                     L"[Waiting for log file; Windows error %lu]\r\n%s",
                     (unsigned long)error, pane->path);
        SetWindowTextW(pane->edit, message);
        HeapFree(GetProcessHeap(), 0, message);
    }
    pane->missing = 1;
    pane->loaded = 0;
}

static wchar_t *decode_log(const unsigned char *bytes, DWORD byte_count)
{
    const unsigned char *start = bytes;
    DWORD count = byte_count;
    wchar_t *text;
    int length;
    UINT codepage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;

    if (count >= 2 && start[0] == 0xff && start[1] == 0xfe) {
        size_t characters = (count - 2) / sizeof(wchar_t);
        text = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   (characters + 1) * sizeof(wchar_t));
        if (text) CopyMemory(text, start + 2, characters * sizeof(wchar_t));
        return text;
    }
    if (count >= 3 && start[0] == 0xef && start[1] == 0xbb && start[2] == 0xbf) {
        start += 3;
        count -= 3;
    }
    if (!count) {
        text = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(wchar_t));
        return text;
    }
    length = MultiByteToWideChar(codepage, flags, (const char *)start, (int)count, NULL, 0);
    if (!length) {
        /* Legacy ANSI logs are displayed as text, never re-created or corrected. */
        codepage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codepage, flags, (const char *)start, (int)count, NULL, 0);
    }
    if (!length) return NULL;
    text = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                               ((size_t)length + 1) * sizeof(wchar_t));
    if (!text) return NULL;
    if (!MultiByteToWideChar(codepage, flags, (const char *)start, (int)count,
                             text, length)) {
        HeapFree(GetProcessHeap(), 0, text);
        return NULL;
    }
    return text;
}

static void refresh_pane(LogPane *pane)
{
    HANDLE file;
    LARGE_INTEGER size;
    FILETIME write_time;
    unsigned char *bytes;
    DWORD to_read, read_count = 0;
    wchar_t *text;

    file = CreateFileW(pane->path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        show_waiting(pane, GetLastError());
        return;
    }
    if (!GetFileSizeEx(file, &size) || !GetFileTime(file, NULL, NULL, &write_time)) {
        DWORD error = GetLastError();
        CloseHandle(file);
        show_waiting(pane, error);
        return;
    }
    if (pane->loaded && size.QuadPart == pane->last_size.QuadPart &&
        CompareFileTime(&write_time, &pane->last_write) == 0) {
        CloseHandle(file);
        return;
    }
    if (size.QuadPart > VIEWER_MAX_BYTES) {
        /* Show the tail of a large file; the limit is disclosed in the UI. */
        LARGE_INTEGER offset;
        offset.QuadPart = size.QuadPart - VIEWER_MAX_BYTES;
        if (!SetFilePointerEx(file, offset, NULL, FILE_BEGIN)) {
            DWORD error = GetLastError();
            CloseHandle(file);
            show_waiting(pane, error);
            return;
        }
        to_read = VIEWER_MAX_BYTES;
    } else {
        to_read = (DWORD)size.QuadPart;
    }
    bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      (size_t)to_read + 2);
    if (!bytes) {
        CloseHandle(file);
        return;
    }
    if (!ReadFile(file, bytes, to_read, &read_count, NULL)) {
        DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, bytes);
        CloseHandle(file);
        show_waiting(pane, error);
        return;
    }
    CloseHandle(file);
    text = decode_log(bytes, read_count);
    HeapFree(GetProcessHeap(), 0, bytes);
    if (!text) return;
    SetWindowTextW(pane->edit, text);
    /* Existing content is loaded from disk verbatim; tail newly appended lines. */
    {
        int length = GetWindowTextLengthW(pane->edit);
        SendMessageW(pane->edit, EM_SETSEL, (WPARAM)length, (LPARAM)length);
        SendMessageW(pane->edit, EM_SCROLLCARET, 0, 0);
    }
    HeapFree(GetProcessHeap(), 0, text);
    pane->last_size = size;
    pane->last_write = write_time;
    pane->loaded = 1;
    pane->missing = 0;
}

static void arrange(HWND window)
{
    RECT client;
    int i, padding = 12, heading_height = 48, gap = 12;
    int width, height, pane_width, pane_height;
    GetClientRect(window, &client);
    width = client.right - client.left;
    height = client.bottom - client.top;
    MoveWindow(heading, padding, 8, width - 2 * padding, heading_height - 8, TRUE);
    pane_width = pane_count == 1 ? width - 2 * padding : (width - 2 * padding - gap) / 2;
    pane_height = pane_count == 1 ? height - heading_height - padding :
        (height - heading_height - padding - gap) / 2;
    for (i = 0; i < pane_count; ++i) {
        int x = padding + (pane_count == 1 ? 0 : i % 2) * (pane_width + gap);
        int y = heading_height + (pane_count == 1 ? 0 : i / 2) * (pane_height + gap);
        MoveWindow(panes[i].label, x, y, pane_width, 23, TRUE);
        MoveWindow(panes[i].edit, x, y + 25, pane_width, pane_height - 25, TRUE);
    }
}

static LRESULT CALLBACK viewer_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
    {
        int i;
        const wchar_t *labels[4] = {
            L"Coordinator / server.log", L"Worker 1 / worker1.log",
            L"Worker 2 / worker2.log", L"Worker 3 / worker3.log"
        };
        white_brush = CreateSolidBrush(RGB(255, 255, 255));
        label_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        log_font = CreateFontW(pane_count == 1 ? -22 : -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        heading = CreateWindowExW(0, L"STATIC", source_description,
                                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  0, 0, 0, 0, window, NULL, NULL, NULL);
        SendMessageW(heading, WM_SETFONT, (WPARAM)label_font, TRUE);
        for (i = 0; i < pane_count; ++i) {
            panes[i].label = CreateWindowExW(0, L"STATIC",
                pane_count == 1 ? L"Actual saved output / read-only" : labels[i],
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window, NULL, NULL, NULL);
            panes[i].edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                0, 0, 0, 0, window, NULL, NULL, NULL);
            SendMessageW(panes[i].label, WM_SETFONT, (WPARAM)label_font, TRUE);
            SendMessageW(panes[i].edit, WM_SETFONT, (WPARAM)log_font, TRUE);
            SendMessageW(panes[i].edit, EM_SETLIMITTEXT, VIEWER_MAX_BYTES, 0);
            refresh_pane(&panes[i]);
        }
        arrange(window);
        SetTimer(window, 1, 500, NULL);
        return 0;
    }
    case WM_SIZE:
        arrange(window);
        return 0;
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *limits = (MINMAXINFO *)lparam;
        limits->ptMinTrackSize.x = 640;
        limits->ptMinTrackSize.y = 420;
        return 0;
    }
    case WM_TIMER:
    {
        int i;
        for (i = 0; i < pane_count; ++i) refresh_pane(&panes[i]);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wparam, RGB(255, 255, 255));
        SetTextColor((HDC)wparam, RGB(0, 0, 0));
        return (LRESULT)white_brush;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        if (log_font) DeleteObject(log_font);
        if (white_brush) DeleteObject(white_brush);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show)
{
    int argc, i, screen_width, screen_height, width, height;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *file_path = NULL, *directory = NULL;
    WNDCLASSEXW cls = {0};
    HWND window;
    MSG message;
    (void)previous;
    (void)command_line;
    if (!argv) return 2;
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--file") && i + 1 < argc) file_path = argv[++i];
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) directory = argv[++i];
        else if (!wcscmp(argv[i], L"--title") && i + 1 < argc)
            wcsncpy_s(window_title, _countof(window_title), argv[++i], _TRUNCATE);
        else {
            MessageBoxW(NULL, L"Usage:\nresult_viewer.exe --file LOG [--title TITLE]\n"
                              L"result_viewer.exe --dir LOG_DIRECTORY [--title TITLE]",
                        L"Socket result viewer", MB_OK | MB_ICONINFORMATION);
            LocalFree(argv);
            return 2;
        }
    }
    if ((!file_path && !directory) || (file_path && directory)) {
        MessageBoxW(NULL, L"Choose exactly one input: --file LOG or --dir LOG_DIRECTORY.",
                    L"Socket result viewer", MB_OK | MB_ICONINFORMATION);
        LocalFree(argv);
        return 2;
    }
    if (file_path) {
        pane_count = 1;
        if (wcsncpy_s(panes[0].path, _countof(panes[0].path), file_path, _TRUNCATE)) {
            LocalFree(argv);
            return 2;
        }
    } else {
        const wchar_t *names[] = { L"server.log", L"worker1.log", L"worker2.log", L"worker3.log" };
        pane_count = 4;
        for (i = 0; i < 4; ++i)
            if (!combine_path(panes[i].path, _countof(panes[i].path), directory, names[i])) {
                LocalFree(argv);
                return 2;
            }
    }
    _snwprintf_s(source_description, _countof(source_description), _TRUNCATE,
        L"Loaded from actual log files; read-only; refresh 500 ms; large logs show last 8 MiB.\r\n%s",
        file_path ? file_path : directory);
    LocalFree(argv);
    SetProcessDPIAware();
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = viewer_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    cls.lpszClassName = L"SocketLabReadOnlyLogViewer";
    if (!RegisterClassExW(&cls)) return 3;
    screen_width = GetSystemMetrics(SM_CXSCREEN);
    screen_height = GetSystemMetrics(SM_CYSCREEN);
    width = screen_width > 1240 ? 1200 : screen_width - 40;
    height = screen_height > (pane_count == 1 ? 620 : 840) ?
        (pane_count == 1 ? 560 : 780) : screen_height - 60;
    window = CreateWindowExW(0, cls.lpszClassName, window_title, WS_OVERLAPPEDWINDOW,
                             (screen_width - width) / 2, (screen_height - height) / 2,
                             width, height, NULL, NULL, instance, NULL);
    if (!window) return 3;
    ShowWindow(window, show);
    UpdateWindow(window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}
