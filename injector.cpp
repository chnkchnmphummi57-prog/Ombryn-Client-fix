// injector.cpp - Ombryn Injector: one button that launches Minecraft, waits for it to
// finish loading (35 s after it starts, for slow PCs), then loads ombryn.dll into it.
// For singleplayer use.
#include <windows.h>
#include <tlhelp32.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cwchar>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

static const int DELAY_SECONDS = 35;     // after Minecraft starts, before injecting
static const int START_TIMEOUT = 90;     // seconds to wait for Minecraft to appear
enum { ID_INJECT = 1, ID_BROWSE = 2, ID_PATH = 3, ID_STATUS = 4, TIMER_ID = 1 };

static HWND g_inject, g_status, g_path;
static std::wstring g_dll;
static int g_left = 0;
enum State { IDLE, WAIT_PROCESS, COUNTDOWN };
static State g_state = IDLE;
static int g_timeout = 0;

static std::wstring DefaultDll() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    wchar_t* s = wcsrchr(p, L'\\');
    if (s) *(s + 1) = 0;
    return std::wstring(p) + L"ombryn.dll";
}

static DWORD FindPid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof pe;
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static void RunHidden(std::wstring cmd) {
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) return;
    WaitForSingleObject(pi.hProcess, 8000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
}

static void SetStatus(const std::wstring& t) { SetWindowTextW(g_status, t.c_str()); }
static void ResetButton() { SetWindowTextW(g_inject, L"Launch Minecraft && Inject"); }

static int ProcessAgeSeconds(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME c, e, k, u; int age = 0;
    if (GetProcessTimes(h, &c, &e, &k, &u)) {
        FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER a, b;
        a.LowPart = c.dwLowDateTime; a.HighPart = c.dwHighDateTime;
        b.LowPart = nowFt.dwLowDateTime; b.HighPart = nowFt.dwHighDateTime;
        if (b.QuadPart > a.QuadPart) age = (int)((b.QuadPart - a.QuadPart) / 10000000ULL);
    }
    CloseHandle(h);
    return age;
}

static bool LaunchMinecraft() {
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"minecraft:", nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) return true;
    r = ShellExecuteW(nullptr, L"open", L"shell:AppsFolder\\Microsoft.MinecraftUWP_8wekyb3d8bbwe!App",
                      nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

// Returns a message for the status line; ok = true if the DLL loaded.
static std::wstring DoInject(bool& ok) {
    ok = false;
    if (GetFileAttributesW(g_dll.c_str()) == INVALID_FILE_ATTRIBUTES)
        return L"Can't find the DLL. Use Browse to pick ombryn.dll.";

    // Bedrock can only load DLLs that "ALL APPLICATION PACKAGES" may read.
    RunHidden(L"icacls \"" + g_dll + L"\" /grant *S-1-15-2-1:(RX) *S-1-15-2-2:(RX)");

    DWORD pid = FindPid(L"Minecraft.Windows.exe");
    if (!pid) return L"Minecraft isn't running. Start the game and a world, then try again.";
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return L"Couldn't open the game (error " + std::to_wstring(GetLastError()) + L"). Try Run as administrator.";

    size_t bytes = (g_dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem || !WriteProcessMemory(proc, mem, g_dll.c_str(), bytes, nullptr)) {
        CloseHandle(proc);
        return L"Couldn't write to the game (error " + std::to_wstring(GetLastError()) + L").";
    }
    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, mem, 0, nullptr);
    if (!th) {
        CloseHandle(proc);
        return L"Couldn't start the load thread (error " + std::to_wstring(GetLastError()) + L").";
    }
    WaitForSingleObject(th, 10000);
    DWORD ret = 0;
    GetExitCodeThread(th, &ret);
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(th); CloseHandle(proc);
    if (!ret) return L"The game refused to load the DLL.";
    ok = true;
    return L"Loaded! Press Esc, then Insert in the game.";
}

static void Browse(HWND owner) {
    wchar_t file[MAX_PATH] = {};
    wcsncpy_s(file, g_dll.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"DLL files\0*.dll\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        g_dll = file;
        SetWindowTextW(g_path, (L"DLL: " + g_dll).c_str());
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        g_dll = DefaultDll();
        g_path = CreateWindowExW(0, L"STATIC", (L"DLL: " + g_dll).c_str(), WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS,
                                 12, 12, 356, 20, hwnd, (HMENU)ID_PATH, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        12, 38, 100, 26, hwnd, (HMENU)ID_BROWSE, hi, nullptr);
        g_inject = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   12, 76, 356, 44, hwnd, (HMENU)ID_INJECT, hi, nullptr);
        g_status = CreateWindowExW(0, L"STATIC", L"Press the button: it starts Minecraft, waits 35 s, then injects.",
                                   WS_CHILD | WS_VISIBLE, 12, 132, 356, 40, hwnd, (HMENU)ID_STATUS, hi, nullptr);
        ResetButton();
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BROWSE && g_state == IDLE) {
            Browse(hwnd);
        } else if (LOWORD(wp) == ID_INJECT) {
            if (g_state != IDLE) {  // cancel
                KillTimer(hwnd, TIMER_ID);
                g_state = IDLE; g_left = 0;
                ResetButton();
                SetStatus(L"Cancelled.");
            } else {
                if (GetFileAttributesW(g_dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    SetStatus(L"Can't find the DLL. Use Browse to pick ombryn.dll.");
                    return 0;
                }
                SetWindowTextW(g_inject, L"Cancel");
                DWORD pid = FindPid(L"Minecraft.Windows.exe");
                if (pid) {  // already running: only wait for whatever is left of the 35 s
                    int left = DELAY_SECONDS - ProcessAgeSeconds(pid);
                    g_left = left < 3 ? 3 : left;
                    g_state = COUNTDOWN;
                    SetStatus(L"Minecraft is already running. Injecting in " + std::to_wstring(g_left) + L" s...");
                } else if (LaunchMinecraft()) {
                    g_state = WAIT_PROCESS;
                    g_timeout = START_TIMEOUT;
                    SetStatus(L"Launching Minecraft...");
                } else {
                    ResetButton();
                    SetStatus(L"Couldn't launch Minecraft. Start it yourself, then press the button.");
                    return 0;
                }
                SetTimer(hwnd, TIMER_ID, 1000, nullptr);
            }
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ID) {
            if (g_state == WAIT_PROCESS) {
                if (FindPid(L"Minecraft.Windows.exe")) {
                    g_state = COUNTDOWN;
                    g_left = DELAY_SECONDS;
                    SetStatus(L"Minecraft started. Injecting in " + std::to_wstring(g_left) + L" s...");
                } else if (--g_timeout <= 0) {
                    KillTimer(hwnd, TIMER_ID);
                    g_state = IDLE;
                    ResetButton();
                    SetStatus(L"Minecraft didn't start. If the launcher opened, press Play, then try again.");
                } else {
                    SetStatus(L"Waiting for Minecraft to start... (press Play if a launcher opened)");
                }
            } else if (g_state == COUNTDOWN) {
                if (--g_left > 0) {
                    SetStatus(L"Injecting in " + std::to_wstring(g_left) + L" s...");
                } else {
                    KillTimer(hwnd, TIMER_ID);
                    g_state = IDLE;
                    SetStatus(L"Injecting...");
                    UpdateWindow(hwnd);
                    bool ok = false;
                    std::wstring msg = DoInject(ok);
                    SetStatus(msg);
                    ResetButton();
                }
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"OmbrynInjector";
    RegisterClassExW(&wc);

    RECT r = {0, 0, 380, 184};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Ombryn Injector", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, hi, nullptr);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
