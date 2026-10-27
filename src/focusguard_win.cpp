// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard - Windows backend.
//
// * Foreground changes arrive through an out-of-context WinEvent hook: no DLL
//   injection, no cost to other processes.
// * Human input is observed with Raw Input (RIDEV_INPUTSINK) instead of
//   low-level hooks, so FocusGuard never sits in the system input path and
//   cannot add input latency. Injected input (SendInput, macro tools) is ignored.
// * When a window takes the foreground without a deliberate user action, the
//   previous window is restored and the intruder's taskbar button flashes.
// * The settings app talks to us over a named pipe (ipc_win.hpp, control.hpp),
//   serviced from the same message loop.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  // Windows 7+
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include "common.hpp"
#include "ipc_win.hpp"

#ifndef EVENT_SYSTEM_DESKTOPSWITCH
#define EVENT_SYSTEM_DESKTOPSWITCH 0x0020
#endif

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
enum : UINT {
    ID_PAUSE = 100, ID_DRYRUN, ID_STRICT, ID_NOTIFY, ID_MINIMIZE,
    ID_STATS, ID_RELOAD, ID_OPENCFG, ID_EXIT
};

fg::Config g_cfg;
fg::IntentTracker g_intent;
fg::FightTracker g_fights;
fg::Matcher g_allow;
fg::Matcher g_block;
fg::Stats g_stats;
fg::NotifyGate g_notify_gate;
std::vector<std::string> g_args;  // for live reload

HWND g_wnd = nullptr;       // hidden window: raw input sink + tray owner
HWND g_current = nullptr;   // last foreground window we accepted
HWND g_expected = nullptr;  // window we are restoring; its activation is our own doing
bool g_in_focus_change = false;                     // reentrancy guard, see win_event_proc
std::vector<std::pair<HWND, DWORD>> g_deferred;     // foreground events that arrived meanwhile
bool g_paused = false;
bool g_tray_added = false;
NOTIFYICONDATAW g_nid{};
UINT g_msg_taskbar_created = 0;
fg::ControlServer g_ctl;  // the settings app's connection
fg::EventLog g_events;
fg::Config g_file_cfg;      // g_cfg as the file and command line give it, before overrides
fg::Overrides g_overrides;  // settings flipped from the tray menu
fg::History g_history;      // activity and statistics on disk
fg::Stats g_all_time;       // statistics across restarts (when history is on)
long long g_since = fg::wall_ms();

// ------------------------------------------------------------------ helpers

std::string narrow(const wchar_t* w, int len) {
    if (!w || len <= 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    return s;
}

std::string narrow(const wchar_t* w) { return w ? narrow(w, (int)wcslen(w)) : std::string(); }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::vector<std::string> utf8_args() {
    std::vector<std::string> out;
    int n = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 0; i < n; ++i) out.push_back(narrow(argv[i]));
        LocalFree(argv);
    }
    return out;
}

std::string default_config_path() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return narrow(buf) + "\\FocusGuard\\focusguard.conf";
}

DWORD window_pid(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}

struct ProcInfo {
    std::string name;  // empty if the process can't be queried
    bool have_age = false;
    uint32_t age_ms = 0;
};

ProcInfo query_process(DWORD pid) {
    ProcInfo pi;
    HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!p) return pi;

    wchar_t path[1024];
    DWORD len = 1024;
    if (QueryFullProcessImageNameW(p, 0, path, &len)) {
        const wchar_t* base = path;
        for (DWORD i = 0; i < len; ++i)
            if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
        pi.name = narrow(base, (int)(path + len - base));
    }

    FILETIME created, exited, kernel, user, now;
    if (GetProcessTimes(p, &created, &exited, &kernel, &user)) {
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER c, t;
        c.LowPart = created.dwLowDateTime;
        c.HighPart = created.dwHighDateTime;
        t.LowPart = now.dwLowDateTime;
        t.HighPart = now.dwHighDateTime;
        if (t.QuadPart >= c.QuadPart) {
            const unsigned long long ms = (t.QuadPart - c.QuadPart) / 10000;  // 100 ns -> ms
            pi.have_age = true;
            pi.age_ms = ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)ms;
        }
    }
    CloseHandle(p);
    return pi;
}

std::string window_title(HWND h) {
    wchar_t buf[256];
    const int n = GetWindowTextW(h, buf, 256);  // reads cached text; never blocks on hung apps
    return narrow(buf, n);
}

std::string window_class(HWND h) {
    wchar_t buf[256];
    const int n = GetClassNameW(h, buf, 256);
    return narrow(buf, n);
}

// The process of the app a window belongs to. Windows Store apps all sit in
// frames owned by ApplicationFrameHost.exe, with the app's own process owning
// a window inside the frame; without this, every Store app would count as the
// "same application" as every other. A suspended Store app may have no such
// window: then 0, which never matches anything.
DWORD app_pid(HWND h) {
    const DWORD pid = window_pid(h);
    if (window_class(h) != "ApplicationFrameWindow") return pid;
    struct Ctx {
        DWORD host, found;
    } ctx{pid, 0};
    EnumChildWindows(
        h,
        [](HWND child, LPARAM lp) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(lp);
            const DWORD p = window_pid(child);
            if (p && p != c.host) {
                c.found = p;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

std::string describe(const std::string& name, const std::string& title) {
    return name + " \"" + fg::ellipsize(title, 60) + "\"";
}

// Windows on another virtual desktop are "cloaked": still visible as far as
// IsWindowVisible is concerned, but restoring one would yank the user back to
// that desktop. DWMWA_CLOAKED (Windows 8+) simply fails on Windows 7.
bool cloaked(HWND h) {
    constexpr DWORD kDwmwaCloaked = 14;  // DWMWA_CLOAKED; missing from older MinGW headers
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, kDwmwaCloaked, &c, sizeof c)) && c != 0;
}

bool usable(HWND h) { return h && IsWindow(h) && IsWindowVisible(h) && !IsIconic(h) && !cloaked(h); }

// Windows only lets the process that received the last input event change the
// foreground. These are the usual, well-behaved ways to get that permission.
bool restore_foreground(HWND target) {
    // 1. A no-op injected mouse event makes us the last-input process.
    INPUT nop{};
    nop.type = INPUT_MOUSE;
    SendInput(1, &nop, sizeof(INPUT));
    if (SetForegroundWindow(target) && GetForegroundWindow() == target) return true;

    // 2. Share input state with the thread that currently owns the foreground.
    HWND fgw = GetForegroundWindow();
    const DWORD fg_thread = fgw ? GetWindowThreadProcessId(fgw, nullptr) : 0;
    const DWORD me = GetCurrentThreadId();
    const bool attached = fg_thread && fg_thread != me && AttachThreadInput(me, fg_thread, TRUE);
    BringWindowToTop(target);
    SetForegroundWindow(target);
    if (attached) AttachThreadInput(me, fg_thread, FALSE);
    if (GetForegroundWindow() == target) return true;

    // 3. Last resort: a synthetic Alt tap also lifts the foreground lock.
    INPUT alt[2]{};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(target);
    return GetForegroundWindow() == target;
}

std::string default_config_path();  // defined below
std::wstring widen(const std::string& s);
void tray_update();
void push_status();
void sync_history();

// ------------------------------------------------------------------ notify

// Show a tray balloon. Falls back silently if there is no tray icon.
void notify(const std::string& app, const std::string& title) {
    if (!g_tray_added) return;
    g_nid.uFlags = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_WARNING;
    lstrcpynW(g_nid.szInfoTitle, L"FocusGuard blocked a window", ARRAYSIZE(g_nid.szInfoTitle));
    const std::string body = app + (title.empty() ? "" : ": " + fg::ellipsize(title, 80)) +
                             " tried to steal focus";
    lstrcpynW(g_nid.szInfo, widen(body).c_str(), ARRAYSIZE(g_nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;  // restore for later NIM_MODIFYs
}

// ------------------------------------------------------------------ config

void rebuild_matchers() {
    g_allow.build(g_cfg.allow);
    g_block.build(g_cfg.block);
}

void reload() {
    fg::Config fresh;
    if (fg::parse_command_line(g_args, fresh, default_config_path()) != fg::ParseResult::Run) {
        fg::logmsg(0, "reload failed: keeping the previous settings");
        return;
    }
    g_file_cfg = fresh;
    g_cfg = std::move(fresh);
    g_overrides.reapply(g_file_cfg, g_cfg);  // tray toggles stay unless the file changed them
    rebuild_matchers();
    fg::apply_logging(g_cfg);
    sync_history();
    tray_update();
    push_status();
    fg::logmsg(1, "settings reloaded - grace %u ms, %u allow, %u block",
               (unsigned)g_cfg.grace_ms, (unsigned)g_allow.size(), (unsigned)g_block.size());
}

// ------------------------------------------------------------------ control

std::string status_json() {
    const std::string path = g_cfg.config_path.empty() ? default_config_path() : g_cfg.config_path;
    return fg::status_json({g_cfg, g_paused, g_stats, g_all_time, g_since, (long long)GetCurrentProcessId(), path,
                            fg::has_cli_overrides(g_args), g_overrides.list()});
}

// Where the history lives: %LOCALAPPDATA%\FocusGuard.
std::string state_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return narrow(buf) + "\\FocusGuard";
}

// Open or close the on-disk history to match the "history" setting.
void sync_history() {
    if (g_cfg.history && !g_history.is_open()) {
        g_events.clear();
        const std::string dir = state_dir();
        if (dir.empty() || !g_history.open(dir, g_events, g_all_time, g_since))
            fg::logmsg(0, "warning: cannot keep the history in '%s'", dir.c_str());
    } else if (!g_cfg.history && g_history.is_open()) {
        g_history.close();
    }
}

void clear_history() {
    g_events.clear();
    g_history.clear_events();
    fg::logmsg(1, "activity history cleared");
}

void reset_stats() {
    g_stats.clear();
    g_all_time.clear();
    g_since = fg::wall_ms();
    g_history.save_stats(g_all_time, g_since);
    fg::logmsg(1, "statistics reset");
}

// Tell connected settings apps that something changed.
void push_status() {
    if (g_ctl.has_subscribers()) g_ctl.broadcast(status_json());
}

void record_event(const char* kind, const std::string& app, const std::string& process, const std::string& cls,
                  const std::string& title, const char* reason) {
    fg::Event e{fg::wall_ms(), kind, app, process, cls, title, reason};
    if (g_ctl.has_subscribers()) g_ctl.broadcast(fg::event_json(e, "event"));
    g_history.append(e);
    g_events.add(std::move(e));
}

// The windows a user would see in the taskbar, for the settings app's picker.
std::vector<fg::WindowInfo> list_windows() {
    struct Ctx {
        std::vector<fg::WindowInfo> out;
        HWND active;
    } ctx{{}, GetForegroundWindow()};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(lp);
            const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            const bool app_window = !GetWindow(h, GW_OWNER) || (ex & WS_EX_APPWINDOW);
            if (!IsWindowVisible(h) || cloaked(h) || (ex & WS_EX_TOOLWINDOW) || !app_window) return TRUE;
            const DWORD pid = window_pid(h);
            if (pid == GetCurrentProcessId()) return TRUE;
            fg::WindowInfo w;
            w.title = window_title(h);
            if (w.title.empty()) return TRUE;
            const DWORD real = app_pid(h);
            w.process = query_process(real ? real : pid).name;
            w.cls = window_class(h);
            w.app = !w.process.empty() ? w.process : w.cls;
            w.active = h == c.active;
            w.allowed = g_allow.match(w.process, "", w.cls, w.title);
            w.blocked = g_block.match(w.process, "", w.cls, w.title);
            c.out.push_back(std::move(w));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

// ------------------------------------------------------------------ core

void on_focus_change(HWND h, DWORD event_time) {
    if (!h) return;
    if (h == g_expected) {  // our own restore landing
        g_expected = nullptr;
        g_current = h;
        return;
    }
    g_expected = nullptr;
    if (h == g_current) return;

    const HWND prev = g_current;
    g_current = h;  // accepted unless we block it below
    if (g_paused || !prev) return;

    // WinEvents are dispatched like sent messages, ahead of queued WM_INPUT, so
    // the click that caused this change may still be waiting in our queue.
    // Process it first or we would block the user's own action.
    MSG m;
    while (PeekMessageW(&m, g_wnd, WM_INPUT, WM_INPUT, PM_REMOVE)) DispatchMessageW(&m);

    const uint32_t now = GetTickCount();
    const uint32_t t = event_time ? (uint32_t)event_time : now;
    const DWORD pid = app_pid(h);  // 0: a Store app we can't see into
    const ProcInfo proc = query_process(pid ? pid : window_pid(h));
    const std::string title = window_title(h);
    const std::string cls = window_class(h);
    const std::string app = !proc.name.empty() ? proc.name : !cls.empty() ? cls : "?";

    fg::Facts f;
    f.user_intent = g_intent.recent(t, g_cfg.grace_ms);
    f.prev_usable = usable(prev);
    f.same_app = g_cfg.allow_same_app && pid != 0 && pid == app_pid(prev);
    f.related = g_cfg.allow_related && GetAncestor(h, GA_ROOTOWNER) == GetAncestor(prev, GA_ROOTOWNER);
    f.launched_by_user =
        g_cfg.allow_launched && fg::launched_by_user(g_intent, proc.have_age, proc.age_ms, now, g_cfg);
    f.allowlisted = g_allow.match(proc.name, "", cls, title);
    f.blocklisted = g_block.match(proc.name, "", cls, title);

    const std::string who = describe(app, title);
    auto record = [&](const char* kind, const char* reason) { record_event(kind, app, proc.name, cls, title, reason); };

    const fg::Decision d = fg::decide(f);
    if (d.allow) {
        if (fg::log_state().level >= 2) fg::logmsg(2, "allowed  %s  (%s)", who.c_str(), d.reason);
        record("allowed", d.reason);
        return;
    }
    if (!g_fights.keep_fighting((uint64_t)(uintptr_t)h, now, g_cfg)) {
        fg::logmsg(1, "gave up  %s  (keeps grabbing focus; letting it win)", who.c_str());
        record("gave-up", "keeps grabbing focus");
        return;
    }
    if (g_cfg.dry_run) {
        fg::logmsg(1, "would block  %s  (%s, dry run)", who.c_str(), d.reason);
        record("would-block", d.reason);
        return;
    }

    g_expected = prev;
    if (restore_foreground(prev)) {
        g_current = prev;
        if (g_cfg.minimize_on_block) ShowWindowAsync(h, SW_MINIMIZE);
        FLASHWINFO fi{};
        fi.cbSize = sizeof(fi);
        fi.hwnd = h;
        fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;  // flash until the user switches to it
        FlashWindowEx(&fi);
        g_stats.record(app);
        g_all_time.record(app);
        g_history.save_stats(g_all_time, g_since);
        fg::logmsg(1, "blocked  %s  (%s)", who.c_str(), d.reason);
        record("blocked", d.reason);
        push_status();  // the statistics changed
        if (g_cfg.notify && g_notify_gate.allow(now, g_cfg)) notify(app, title);
    } else {
        g_expected = nullptr;
        fg::logmsg(1, "FAILED   %s  (Windows refused to restore focus; try running as administrator)",
                   who.c_str());
        record("failed", "Windows refused to restore focus");
    }
}

void CALLBACK win_event_proc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG, DWORD,
                             DWORD time) {
    if (event == EVENT_SYSTEM_DESKTOPSWITCH) {
        // Returning from UAC / lock screen / Ctrl+Alt+Del: input there is
        // invisible to us, so treat the switch itself as a user action.
        g_intent.mark(time ? (uint32_t)time : (uint32_t)GetTickCount());
    } else if (event == EVENT_SYSTEM_FOREGROUND && id_object == OBJID_WINDOW) {
        // PeekMessage in on_focus_change can deliver further WinEvents; queue
        // them so each change is judged in order instead of recursively.
        if (g_in_focus_change) {
            g_deferred.emplace_back(hwnd, time);
            return;
        }
        g_in_focus_change = true;
        on_focus_change(hwnd, time);
        for (size_t i = 0; i < g_deferred.size(); ++i) {
            const auto ev = g_deferred[i];  // copy: the vector may grow meanwhile
            on_focus_change(ev.first, ev.second);
        }
        g_deferred.clear();
        g_in_focus_change = false;
    }
}

void on_raw_input(HRAWINPUT handle, uint32_t t) {
    alignas(8) static BYTE buf[1024];  // one syscall per event; big enough for touch HID reports
    UINT size = sizeof(buf);
    if (GetRawInputData(handle, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    const RAWINPUT& ri = *reinterpret_cast<const RAWINPUT*>(buf);
    if (!ri.header.hDevice) return;  // injected via SendInput: not a human

    switch (ri.header.dwType) {
        case RIM_TYPEMOUSE: {
            constexpr USHORT kDown = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
                                     RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN;
            if (ri.data.mouse.usButtonFlags & kDown)
                g_intent.mark(t);
            else if (g_cfg.count_motion && (ri.data.mouse.lLastX || ri.data.mouse.lLastY))
                g_intent.touch(t);
            break;
        }
        case RIM_TYPEKEYBOARD: {
            const RAWKEYBOARD& kb = ri.data.keyboard;
            if (kb.VKey == 0xFF) return;  // fake key from an escape sequence
            const bool down = !(kb.Flags & RI_KEY_BREAK);
            fg::Key k = fg::Key::Other;
            switch (kb.VKey) {
                case VK_RETURN: k = fg::Key::Enter; break;
                case VK_ESCAPE: k = fg::Key::Escape; break;
                case VK_MENU: case VK_LMENU: case VK_RMENU: k = fg::Key::Alt; break;
                case VK_LWIN: case VK_RWIN: k = fg::Key::Super; break;
                default: break;
            }
            bool mods = false;
            if (down && k == fg::Key::Other)
                mods = ((GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
            if (fg::key_is_intent(k, down, mods)) g_intent.mark(t);
            break;
        }
        case RIM_TYPEHID:  // touch screen
            g_intent.touch(t);
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------ tray

// The shield icons from the resources (src/focusguard.rc.in), at the tray's
// size; builds without the resource file get Windows' stock icons instead.
HICON tray_icon(int id, LPCWSTR fallback) {
    const int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    if (HANDLE h = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), IMAGE_ICON, cx, cy, LR_SHARED))
        return static_cast<HICON>(h);
    return LoadIconW(nullptr, fallback);
}

void tray_remove() {
    if (g_tray_added) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_tray_added = false;
}

void tray_update() {
    if (!g_wnd) return;
    if (!g_cfg.tray) {  // e.g. "tray = no" after a reload
        tray_remove();
        return;
    }
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_paused ? tray_icon(2, IDI_WARNING) : g_cfg.dry_run ? tray_icon(3, IDI_SHIELD) : tray_icon(1, IDI_SHIELD);
    const wchar_t* tip = g_paused ? L"FocusGuard - paused (click to resume)"
                         : g_cfg.dry_run ? L"FocusGuard - dry run (logging only)"
                                         : L"FocusGuard - protecting focus";
    lstrcpynW(g_nid.szTip, tip, ARRAYSIZE(g_nid.szTip));
    g_tray_added = Shell_NotifyIconW(g_tray_added ? NIM_MODIFY : NIM_ADD, &g_nid) != FALSE;
}

void set_paused(bool paused) {
    g_paused = paused;
    if (!paused) {  // resync, but never adopt our own hidden window
        HWND fgw = GetForegroundWindow();
        if (fgw && window_pid(fgw) != GetCurrentProcessId()) g_current = fgw;
        g_expected = nullptr;
    }
    fg::logmsg(1, "%s", paused ? "paused" : "resumed");
    tray_update();
    push_status();
}

void open_config() {
    const std::string path = g_cfg.config_path.empty() ? default_config_path() : g_cfg.config_path;
    if (path.empty()) return;
    const std::wstring wpath = widen(path);
    // Make sure the folder and file exist, then open in the default editor.
    const std::filesystem::path p = std::filesystem::u8path(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (!std::filesystem::exists(p, ec)) {
        if (FILE* f = fg::open_utf8(path, "a")) std::fclose(f);
    }
    // .conf has no default program on a stock Windows install, in which case
    // ShellExecute fails (returns <= 32); use Notepad then.
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        const std::wstring quoted = L"\"" + wpath + L"\"";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

// Open the settings app if it sits next to this executable; otherwise the
// settings file in a text editor.
void open_settings() {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::filesystem::path app = std::filesystem::path(self).parent_path() / L"focusguard-settings.exe";
        std::error_code ec;
        if (std::filesystem::exists(app, ec) &&
            (INT_PTR)ShellExecuteW(nullptr, L"open", app.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32)
            return;
    }
    open_config();
}

// A tray menu toggle: kept across reloads as an override (see fg::Overrides).
void flip(fg::Toggle t, const char* what) {
    g_overrides.flip(t, g_file_cfg, g_cfg);
    fg::logmsg(1, "%s %s", what, fg::toggle_value(g_cfg, t) ? "on" : "off");
}

void tray_menu() {
    const bool strict = !(g_cfg.allow_same_app && g_cfg.allow_launched && g_cfg.allow_related);
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), ID_PAUSE, L"Paused");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (strict ? MF_CHECKED : 0), ID_STRICT, L"Strict mode");
    AppendMenuW(m, MF_STRING | (g_cfg.minimize_on_block ? MF_CHECKED : 0), ID_MINIMIZE, L"Minimise blocked windows");
    AppendMenuW(m, MF_STRING | (g_cfg.notify ? MF_CHECKED : 0), ID_NOTIFY, L"Notifications");
    AppendMenuW(m, MF_STRING | (g_cfg.dry_run ? MF_CHECKED : 0), ID_DRYRUN, L"Dry run (log only)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_STATS, L"Show statistics");
    AppendMenuW(m, MF_STRING, ID_OPENCFG, L"Settings\u2026");
    AppendMenuW(m, MF_STRING, ID_RELOAD, L"Reload settings");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);  // required so the menu closes when clicking elsewhere
    const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
        case ID_PAUSE: set_paused(!g_paused); break;
        case ID_STRICT: flip(fg::Toggle::Strict, "strict mode"); break;
        case ID_MINIMIZE: flip(fg::Toggle::Minimize, "minimise-on-block"); break;
        case ID_NOTIFY: flip(fg::Toggle::Notify, "notifications"); break;
        case ID_DRYRUN:
            flip(fg::Toggle::DryRun, "dry run");
            tray_update();
            break;
        case ID_STATS: {
            g_stats.dump("");
            g_nid.uFlags = NIF_INFO;
            g_nid.dwInfoFlags = NIIF_INFO;
            lstrcpynW(g_nid.szInfoTitle, L"FocusGuard statistics", ARRAYSIZE(g_nid.szInfoTitle));
            lstrcpynW(g_nid.szInfo, widen(g_stats.top_line()).c_str(), ARRAYSIZE(g_nid.szInfo));
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
            break;
        }
        case ID_OPENCFG: open_settings(); break;
        case ID_RELOAD: reload(); break;
        case ID_EXIT: PostMessageW(g_wnd, WM_CLOSE, 0, 0); break;
        default: break;
    }
    if (cmd == ID_STRICT || cmd == ID_MINIMIZE || cmd == ID_NOTIFY || cmd == ID_DRYRUN) push_status();
}

// ------------------------------------------------------------------ window

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INPUT:
            on_raw_input(reinterpret_cast<HRAWINPUT>(lp), (uint32_t)GetMessageTime());
            break;  // DefWindowProc must still run for WM_INPUT
        case WM_TRAYICON:
            if (LOWORD(lp) == WM_LBUTTONUP) set_paused(!g_paused);
            else if (LOWORD(lp) == WM_RBUTTONUP) tray_menu();
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            tray_remove();
            PostQuitMessage(0);
            return 0;
        default:
            if (g_msg_taskbar_created && msg == g_msg_taskbar_created) {  // Explorer restarted
                g_tray_added = false;
                tray_update();
                return 0;
            }
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

BOOL WINAPI console_handler(DWORD type) {
    if (g_wnd) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    // For window-close/logoff/shutdown Windows kills us once we return; give
    // the main thread a moment to remove the tray icon.
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) Sleep(1000);
    return TRUE;
}

}  // namespace

int main() {
    g_args = utf8_args();
    switch (fg::parse_command_line(g_args, g_cfg, default_config_path())) {
        case fg::ParseResult::ExitOk: return 0;
        case fg::ParseResult::ExitError: return 2;
        case fg::ParseResult::Run: break;
    }
    fg::apply_logging(g_cfg);
    rebuild_matchers();
    g_file_cfg = g_cfg;

    HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\FocusGuard.SingleInstance");
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        fg::logmsg(0, "FocusGuard is already running.");
        return 1;
    }
    sync_history();

    if (g_cfg.background) {
        if (!g_cfg.tray) fg::logmsg(0, "note: --background with --no-tray: stop it from Task Manager");
        FreeConsole();
    }
    // React before the user's next keystroke lands in the intruder.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FocusGuardWindow";
    if (!RegisterClassExW(&wc)) {
        fg::logmsg(0, "error: RegisterClassEx failed (%lu)", GetLastError());
        return 1;
    }
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"FocusGuard", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, inst, nullptr);
    if (!g_wnd) {
        fg::logmsg(0, "error: CreateWindowEx failed (%lu)", GetLastError());
        return 1;
    }

    RAWINPUTDEVICE rid[2]{};
    rid[0].usUsagePage = 0x01;  // generic desktop
    rid[0].usUsage = 0x02;      // mouse
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = g_wnd;
    rid[1] = rid[0];
    rid[1].usUsage = 0x06;      // keyboard
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fg::logmsg(0, "error: RegisterRawInputDevices failed (%lu)", GetLastError());
        return 1;
    }
    RAWINPUTDEVICE touch{};
    touch.usUsagePage = 0x0D;   // digitizer
    touch.usUsage = 0x04;       // touch screen (optional; ignore failure)
    touch.dwFlags = RIDEV_INPUTSINK;
    touch.hwndTarget = g_wnd;
    RegisterRawInputDevices(&touch, 1, sizeof(RAWINPUTDEVICE));

    const DWORD hook_flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    HWINEVENTHOOK fg_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                            win_event_proc, 0, 0, hook_flags);
    HWINEVENTHOOK desk_hook = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, nullptr,
                                              win_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!fg_hook) {
        fg::logmsg(0, "error: SetWinEventHook failed (%lu)", GetLastError());
        return 1;
    }

    g_current = GetForegroundWindow();
    g_msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_wnd, g_msg_taskbar_created, MSGFLT_ALLOW, nullptr);  // when elevated
    tray_update();
    SetConsoleCtrlHandler(console_handler, TRUE);

    const bool strict = !(g_cfg.allow_same_app && g_cfg.allow_launched && g_cfg.allow_related);
    fg::logmsg(1, "FocusGuard %s running%s%s - grace %u ms, launch grace %u ms, %u allow, %u block%s%s",
               fg::kVersion, g_cfg.dry_run ? " (DRY RUN)" : "", strict ? " (strict)" : "",
               (unsigned)g_cfg.grace_ms, (unsigned)g_cfg.launch_grace_ms, (unsigned)g_allow.size(),
               (unsigned)g_block.size(), g_cfg.config_path.empty() ? "" : ", config ",
               g_cfg.config_path.c_str());
    fg::logmsg(1, "Ctrl+C or the tray menu quits; left-click the tray icon to pause.");

    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const std::wstring pipe_name = L"\\\\.\\pipe\\" + widen(fg::control_endpoint(session));
    if (!g_ctl.start(pipe_name))
        fg::logmsg(0, "warning: cannot create the control pipe (%lu); the settings app can't connect",
                   GetLastError());
    const fg::ControlHooks control_hooks{
        status_json,
        [] { return fg::windows_json(list_windows()); },
        [](bool pause) {
            if (pause != g_paused) set_paused(pause);
        },
        reload,
        [] { PostMessageW(g_wnd, WM_CLOSE, 0, 0); },
        clear_history,
        [] {
            reset_stats();
            push_status();
        },
    };
    auto service_pipe = [&] {
        g_ctl.poll([&](const std::string& line, bool& subscribe) {
            return fg::handle_control(line, g_events, control_hooks, subscribe);
        });
    };

    // A message loop that also wakes for pipe I/O. MWMO_INPUTAVAILABLE: don't
    // sleep while messages that arrived earlier are still queued.
    bool running = true;
    while (running) {
        std::vector<HANDLE> handles;
        g_ctl.handles(handles);
        MsgWaitForMultipleObjectsEx((DWORD)handles.size(), handles.data(), INFINITE, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        service_pipe();
    }
    g_ctl.stop();

    g_stats.dump(" this session");
    UnhookWinEvent(fg_hook);
    if (desk_hook) UnhookWinEvent(desk_hook);
    tray_remove();
    if (instance) CloseHandle(instance);
    fg::logmsg(1, "FocusGuard stopped.");
    return 0;
}
