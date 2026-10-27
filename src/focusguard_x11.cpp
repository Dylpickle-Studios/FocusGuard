// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard - Linux (X11) backend.
//
// * Active-window changes come from PropertyNotify on the root window's
//   _NET_ACTIVE_WINDOW (EWMH), which every mainstream window manager publishes.
// * Human input comes from XInput2 raw events on the root window: delivered even
//   while another client holds a grab, never interfering with anyone's input.
//   Synthetic XTEST input (xdotool & co.) is ignored.
// * Blocked windows are refused by asking the WM to re-activate the previous
//   window, and get _NET_WM_STATE_DEMANDS_ATTENTION so their taskbar entry flashes.
// * The tray icon and notifications use D-Bus (tray_sni.hpp) when built with
//   FOCUSGUARD_TRAY; the session bus joins the same poll() loop, as does the
//   control socket the settings app talks to (ipc_unix.hpp, control.hpp).
// * The process sleeps in poll() between events: zero CPU while idle.

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/keysym.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>

#include <filesystem>
#include <unordered_set>

#include "common.hpp"
#include "ipc_unix.hpp"
#include "tray_xembed.hpp"
#ifdef FOCUSGUARD_TRAY
#include "tray_sni.hpp"
#endif

namespace {

fg::Config g_cfg;
fg::IntentTracker g_intent;
fg::FightTracker g_fights;
fg::Matcher g_allow;
fg::Matcher g_block;
fg::Stats g_stats;
fg::NotifyGate g_notify_gate;
std::vector<std::string> g_args;  // for live reload

Display* g_dpy = nullptr;
Window g_root = 0;
int g_xi_opcode = -1;
std::unordered_set<int> g_xtest_devices;

std::array<fg::Key, 256> g_keymap{};  // keycode -> key class
std::array<bool, 256> g_held{};       // Alt/Super keycodes currently down
int g_mods_held = 0;

Window g_current = 0;   // last active window we accepted
Window g_expected = 0;  // window we asked the WM to re-activate
bool g_paused = false;
long long g_resume_at = 0;  // a timed pause ends then (ms since the epoch); 0: none
int g_sigpipe[2] = {-1, -1};
bool g_xerror = false;
#ifdef FOCUSGUARD_TRAY
fg::DesktopBus g_bus;
#endif
fg::XEmbedTray g_xembed;  // tray icon for XEmbed trays (when no StatusNotifierItem host)
bool g_warned_no_tray = false;
fg::ControlServer g_ctl;  // the settings app's connection
fg::EventLog g_events;
fg::Config g_file_cfg;     // g_cfg as the file and command line give it, before overrides
fg::Overrides g_overrides;  // settings flipped from the tray menu
fg::History g_history;      // activity and statistics on disk
fg::Stats g_all_time;       // statistics across restarts (when history is on)
long long g_since = fg::wall_ms();

struct Atoms {
    Atom active, wm_pid, net_wm_name, utf8, wm_state, demands_attention, hidden, client_list, wm_type,
        type_dock, type_desktop;
} A;

int on_xerror(Display*, XErrorEvent*) {  // windows vanish all the time; that's fine
    g_xerror = true;
    return 0;
}

// ------------------------------------------------------------------ X helpers

template <class F>
bool read_prop(Window w, Atom prop, Atom type, long max_items, F&& fn) {
    Atom actual = None;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    g_xerror = false;
    const int rc = XGetWindowProperty(g_dpy, w, prop, 0, max_items, False, type, &actual, &format, &n,
                                      &after, &data);
    const bool ok = rc == Success && !g_xerror && data && n > 0;
    if (ok) fn(data, n, format);
    if (data) XFree(data);
    return ok;
}

Window active_window(bool* exists = nullptr) {
    Window w = 0;
    const bool ok = read_prop(g_root, A.active, XA_WINDOW, 1, [&](unsigned char* d, unsigned long, int fmt) {
        if (fmt == 32) w = (Window) reinterpret_cast<unsigned long*>(d)[0];
    });
    if (exists) *exists = ok;
    return w;
}

unsigned long window_pid(Window w) {
    unsigned long pid = 0;
    read_prop(w, A.wm_pid, XA_CARDINAL, 1, [&](unsigned char* d, unsigned long, int fmt) {
        if (fmt == 32) pid = reinterpret_cast<unsigned long*>(d)[0];
    });
    return pid;
}

void wm_class(Window w, std::string& instance, std::string& cls) {
    XClassHint h{};
    g_xerror = false;
    if (XGetClassHint(g_dpy, w, &h) && !g_xerror) {
        if (h.res_name) instance = h.res_name;
        if (h.res_class) cls = h.res_class;
    }
    if (h.res_name) XFree(h.res_name);
    if (h.res_class) XFree(h.res_class);
}

std::string window_title(Window w) {
    std::string t;
    read_prop(w, A.net_wm_name, A.utf8, 256, [&](unsigned char* d, unsigned long n, int fmt) {
        if (fmt == 8) t.assign(reinterpret_cast<char*>(d), n);
    });
    if (t.empty()) {
        char* name = nullptr;
        g_xerror = false;
        if (XFetchName(g_dpy, w, &name) && !g_xerror && name) t = name;
        if (name) XFree(name);
    }
    return t;
}

// Still on screen, not minimised, not on another workspace?
bool usable(Window w) {
    XWindowAttributes a;
    g_xerror = false;
    if (!XGetWindowAttributes(g_dpy, w, &a) || g_xerror || a.map_state != IsViewable) return false;
    bool hidden = false;
    read_prop(w, A.wm_state, XA_ATOM, 64, [&](unsigned char* d, unsigned long n, int fmt) {
        if (fmt != 32) return;
        const unsigned long* atoms = reinterpret_cast<unsigned long*>(d);
        for (unsigned long i = 0; i < n; ++i)
            if (atoms[i] == A.hidden) hidden = true;
    });
    return !hidden;
}

bool transient_for(Window w, Window parent) {
    Window t = 0;
    g_xerror = false;
    return XGetTransientForHint(g_dpy, w, &t) && !g_xerror && t == parent;
}

void send_wm_message(Window w, Atom type, long l0, long l1, long l2, long l3) {
    XEvent e{};
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = type;
    e.xclient.format = 32;
    e.xclient.data.l[0] = l0;
    e.xclient.data.l[1] = l1;
    e.xclient.data.l[2] = l2;
    e.xclient.data.l[3] = l3;
    XSendEvent(g_dpy, g_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

// ------------------------------------------------------------------ /proc

struct ProcInfo {
    std::string comm;
    bool have_age = false;
    uint32_t age_ms = 0;
};

ProcInfo query_process(unsigned long pid) {
    ProcInfo pi;
    if (!pid) return pi;
    const std::string base = "/proc/" + std::to_string(pid);

    if (FILE* f = std::fopen((base + "/comm").c_str(), "r")) {
        char buf[64];
        if (std::fgets(buf, sizeof buf, f)) pi.comm = fg::trim(buf);
        std::fclose(f);
    }
    // The kernel truncates comm to 15 bytes ("gnome-control-c"); recover the
    // full name from the executable so long process names can be listed.
    if (pi.comm.size() == 15) {
        char exe[4096];
        const ssize_t n = readlink((base + "/exe").c_str(), exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = '\0';
            const char* slash = std::strrchr(exe, '/');
            const std::string name = slash ? slash + 1 : exe;
            if (name.compare(0, 15, pi.comm) == 0) pi.comm = name;
        }
    }

    if (FILE* f = std::fopen((base + "/stat").c_str(), "r")) {
        char buf[1024];
        const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
        buf[n] = '\0';
        // comm may contain spaces/parens: fields resume after the last ')'.
        // Field 3 follows it; field 22 is the start time in clock ticks since boot.
        if (const char* s = std::strrchr(buf, ')')) {
            ++s;
            unsigned long long start = 0;
            for (int field = 3; *s; ++field) {
                while (*s == ' ') ++s;
                if (field == 22) {
                    start = std::strtoull(s, nullptr, 10);
                    break;
                }
                while (*s && *s != ' ') ++s;
            }
            const long hz = sysconf(_SC_CLK_TCK);
            timespec ts{};
            if (start && hz > 0 && clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
                const double now_ms = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
                const double start_ms = (double)start * 1000.0 / (double)hz;
                if (now_ms >= start_ms) {
                    const double age = now_ms - start_ms;
                    pi.have_age = true;
                    pi.age_ms = age > 4.0e9 ? 0xFFFFFFFFu : (uint32_t)age;
                }
            }
        }
    }
    return pi;
}

std::string default_config_path();  // defined below

// ------------------------------------------------------------------ notify

void show_window(Window w);                // defined below
void allow_app(const std::string& entry);  // defined below

// Desktop notification about a blocked window: over D-Bus when connected,
// with "Show it" and "Always allow" buttons; otherwise via notify-send (part
// of libnotify's tools). Children are auto-reaped (SA_NOCLDWAIT) so no zombies
// accumulate.
void notify(const std::string& app, const std::string& title, Window w, const std::string& entry) {
    const std::string body = app + (title.empty() ? "" : ": " + fg::ellipsize(title, 80)) +
                             "\ntried to steal focus";
#ifdef FOCUSGUARD_TRAY
    if (g_bus.connected()) {
        std::vector<fg::DesktopBus::NotifyButton> buttons = {{"show", "Show it"}};
        if (!entry.empty()) buttons.push_back({"allow", "Always allow"});
        g_bus.notify("FocusGuard blocked a window", body, "dialog-warning", buttons,
                     [w, entry](const std::string& key) {
                         if (key == "allow") allow_app(entry);
                         else show_window(w);  // "show", or the notification itself
                     });
        return;
    }
#else
    (void)w;
    (void)entry;
#endif
    const pid_t pid = fork();
    if (pid != 0) return;  // parent (or fork failure): carry on
    execlp("notify-send", "notify-send", "-a", "FocusGuard", "-i", "dialog-warning",
           "-u", "low", "FocusGuard blocked a window", body.c_str(), (char*)nullptr);
    _exit(127);  // notify-send not installed: silently give up in the child
}

// ------------------------------------------------------------------ config

void rebuild_matchers() {
    g_allow.build(g_cfg.allow);
    g_block.build(g_cfg.block);
}

void select_input_events();  // defined below
void sync_bus(bool may_connect);  // defined below
void sync_history();  // defined below

void reload() {
    fg::Config fresh;
    switch (fg::parse_command_line(g_args, fresh, default_config_path())) {
        case fg::ParseResult::Run:
            break;
        default:  // a bad edit shouldn't take the daemon down
            fg::logmsg(0, "reload failed: keeping the previous settings");
            return;
    }
    const bool motion_changed = fresh.count_motion != g_cfg.count_motion;
    g_file_cfg = fresh;
    g_cfg = std::move(fresh);
    g_overrides.reapply(g_file_cfg, g_cfg);  // tray toggles stay unless the file changed them
    rebuild_matchers();
    fg::apply_logging(g_cfg);
    if (motion_changed) select_input_events();
    sync_history();
    sync_bus(true);
    fg::logmsg(1, "settings reloaded - grace %u ms, %u allow, %u block%s%s%s",
               (unsigned)g_cfg.grace_ms, (unsigned)g_allow.size(), (unsigned)g_block.size(),
               g_cfg.allow_same_app && g_cfg.allow_launched && g_cfg.allow_related ? "" : ", strict",
               g_cfg.dry_run ? ", dry-run" : "", g_cfg.notify ? ", notify" : "");
}

// ------------------------------------------------------------------ control

std::string status_json() {
    const std::string path = g_cfg.config_path.empty() ? default_config_path() : g_cfg.config_path;
    return fg::status_json({g_cfg, g_paused, g_resume_at, g_stats, g_all_time, g_since, getpid(), path,
                            fg::has_cli_overrides(g_args), g_overrides.list()});
}

// Where the history lives: $XDG_STATE_HOME/focusguard, else ~/.local/state/focusguard.
std::string state_dir() {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/focusguard";
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/.local/state/focusguard" : std::string();
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
                  const std::string& title, const char* reason, long long after_ms) {
    fg::Event e{fg::wall_ms(), kind, app, process, cls, title, reason, after_ms};
    if (g_ctl.has_subscribers()) g_ctl.broadcast(fg::event_json(e, "event"));
    g_history.append(e);
    g_events.add(std::move(e));
}

// Top-level windows the WM manages, for the settings app's window picker.
std::vector<fg::WindowInfo> list_windows() {
    std::vector<Window> ws;
    read_prop(g_root, A.client_list, XA_WINDOW, 4096, [&](unsigned char* d, unsigned long n, int fmt) {
        if (fmt == 32)
            for (unsigned long i = 0; i < n; ++i) ws.push_back((Window) reinterpret_cast<unsigned long*>(d)[i]);
    });
    const Window active = active_window();
    std::vector<fg::WindowInfo> out;
    for (Window w : ws) {
        bool skip = false;  // panels and the desktop aren't windows anyone would list
        read_prop(w, A.wm_type, XA_ATOM, 16, [&](unsigned char* d, unsigned long n, int fmt) {
            if (fmt != 32) return;
            for (unsigned long i = 0; i < n; ++i) {
                const unsigned long t = reinterpret_cast<unsigned long*>(d)[i];
                skip = skip || t == A.type_dock || t == A.type_desktop;
            }
        });
        if (skip) continue;
        fg::WindowInfo wi;
        wi.process = query_process(window_pid(w)).comm;
        wm_class(w, wi.instance, wi.cls);
        wi.title = window_title(w);
        wi.app = !wi.cls.empty() ? wi.cls : !wi.process.empty() ? wi.process : wi.instance;
        wi.active = w == active;
        wi.allowed = g_allow.match(wi.process, wi.instance, wi.cls, wi.title);
        wi.blocked = g_block.match(wi.process, wi.instance, wi.cls, wi.title);
        out.push_back(std::move(wi));
    }
    return out;
}

// ------------------------------------------------------------------ core

void on_active_changed(Time when) {
    const Window w = active_window();
    if (!w || w == g_root) return;  // focus went nowhere: keep remembering the last real window
    if (w == g_expected) {          // our own restore landing
        g_expected = 0;
        g_current = w;
        return;
    }
    g_expected = 0;
    if (w == g_current) return;

    const Window prev = g_current;
    g_current = w;  // accepted unless we block it below
    if (g_paused || !prev) return;

    const uint32_t t = (uint32_t)when;
    const unsigned long pid = window_pid(w), prev_pid = window_pid(prev);
    const ProcInfo proc = query_process(pid);
    std::string inst, cls;
    wm_class(w, inst, cls);

    const std::string title = window_title(w);

    fg::Facts f;
    f.user_intent = g_intent.recent(t, g_cfg.grace_ms);
    f.prev_usable = usable(prev);
    bool same_app;
    if (pid && prev_pid) {
        same_app = pid == prev_pid;
    } else {  // no _NET_WM_PID (e.g. remote clients): fall back to WM_CLASS
        std::string pinst, pcls;
        wm_class(prev, pinst, pcls);
        same_app = !cls.empty() && cls == pcls;
    }
    f.same_app = same_app && g_cfg.allow_same_app;
    f.related = transient_for(w, prev) && g_cfg.allow_related;
    f.launched_by_user =
        g_cfg.allow_launched && fg::launched_by_user(g_intent, proc.have_age, proc.age_ms, t, g_cfg);
    f.allowlisted = g_allow.match(proc.comm, inst, cls, title);
    f.blocklisted = g_block.match(proc.comm, inst, cls, title);

    const std::string app = !cls.empty() ? cls : !proc.comm.empty() ? proc.comm : inst.empty() ? "?" : inst;
    auto who = [&] { return app + " \"" + fg::ellipsize(title, 60) + "\""; };

    const long long after = g_intent.since_last(t);
    auto record = [&](const char* kind, const char* reason) {
        record_event(kind, app, proc.comm, cls, title, reason, after);
    };

    const fg::Decision d = fg::decide(f);
    if (d.allow) {
        if (f.user_intent) g_intent.switched(t);  // apps launched before this lose their launch pass
        if (fg::log_state().level >= 2) fg::logmsg(2, "allowed  %s  (%s)", who().c_str(), d.reason);
        record("allowed", d.reason);
        return;
    }
    if (!g_fights.keep_fighting((uint64_t)w, t, g_cfg)) {
        fg::logmsg(1, "gave up  %s  (keeps grabbing focus; letting it win)", who().c_str());
        record("gave-up", "keeps grabbing focus");
        return;
    }
    if (g_cfg.dry_run) {
        fg::logmsg(1, "would block  %s  (%s, dry run)", who().c_str(), d.reason);
        record("would-block", d.reason);
        return;
    }

    // Source indication 2 = pager/taskbar: WMs treat it as a user request and comply.
    g_expected = prev;
    g_current = prev;
    send_wm_message(prev, A.active, 2, (long)when, 0, 0);
    send_wm_message(w, A.wm_state, 1 /* add */, (long)A.demands_attention, 0, 2);
    if (g_cfg.minimize_on_block) XIconifyWindow(g_dpy, w, DefaultScreen(g_dpy));
    // Sync rather than flush: if `w` has just vanished, the BadWindow from these
    // requests arrives now instead of poisoning the next property read.
    XSync(g_dpy, False);

    g_stats.record(app);
    g_all_time.record(app);
    g_history.save_stats(g_all_time, g_since);
    fg::logmsg(1, "blocked  %s  (%s)", who().c_str(), d.reason);
    record("blocked", d.reason);
    push_status();  // the statistics changed
    if (g_cfg.notify && g_notify_gate.allow(t, g_cfg)) notify(app, title, w, !cls.empty() ? cls : proc.comm);
}

void build_keymap() {
    g_keymap.fill(fg::Key::Other);
    int lo = 0, hi = 0;
    XDisplayKeycodes(g_dpy, &lo, &hi);
    for (int kc = lo; kc <= hi && kc < 256; ++kc) {
        switch (XkbKeycodeToKeysym(g_dpy, (KeyCode)kc, 0, 0)) {
            case XK_Return: case XK_KP_Enter: case XK_ISO_Enter:
                g_keymap[kc] = fg::Key::Enter; break;
            case XK_Escape:
                g_keymap[kc] = fg::Key::Escape; break;
            case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R:
                g_keymap[kc] = fg::Key::Alt; break;
            case XK_Super_L: case XK_Super_R: case XK_Hyper_L: case XK_Hyper_R:
                g_keymap[kc] = fg::Key::Super; break;
            default: break;
        }
    }
}

void on_raw_key(int keycode, bool down, uint32_t t) {
    if (keycode < 0 || keycode > 255) return;
    const fg::Key k = g_keymap[keycode];
    if ((k == fg::Key::Alt || k == fg::Key::Super) && g_held[keycode] != down) {
        g_held[keycode] = down;
        g_mods_held = std::max(0, g_mods_held + (down ? 1 : -1));
    }

    bool mods = false;
    if (down && k == fg::Key::Other && g_mods_held > 0) {
        // Our tracking says Alt/Super is held; confirm with the server so a
        // missed release (VT switch, screen locker) can't leave it stuck.
        XkbStateRec st;
        mods = XkbGetState(g_dpy, XkbUseCoreKbd, &st) != Success || (st.mods & (Mod1Mask | Mod4Mask));
        if (!mods) {
            g_held.fill(false);
            g_mods_held = 0;
        }
    }
    if (fg::key_is_intent(k, down, mods)) g_intent.mark(t);
}

void on_raw_event(const XIRawEvent& re) {
    if (g_xtest_devices.count(re.sourceid)) return;  // synthetic input, not a human
    const uint32_t t = (uint32_t)re.time;
    switch (re.evtype) {
        case XI_RawButtonPress:
            if (re.detail < 4 || re.detail > 7) g_intent.mark(t);  // 4-7 = scroll wheel
            break;
#ifdef XI_RawTouchBegin
        case XI_RawTouchBegin:
            g_intent.mark(t);
            break;
#endif
        case XI_RawMotion:
            g_intent.touch(t);
            break;
        case XI_RawKeyPress:
            on_raw_key(re.detail, true, t);
            break;
        case XI_RawKeyRelease:
            on_raw_key(re.detail, false, t);
            break;
        default:
            break;
    }
}

void handle_event(XEvent& ev) {
    if (g_xembed.handle(ev)) return;
    switch (ev.type) {
        case GenericEvent: {
            XGenericEventCookie* c = &ev.xcookie;
            if (c->extension == g_xi_opcode && XGetEventData(g_dpy, c)) {
                on_raw_event(*static_cast<XIRawEvent*>(c->data));
                XFreeEventData(g_dpy, c);
            }
            break;
        }
        case PropertyNotify:
            if (ev.xproperty.window == g_root && ev.xproperty.atom == A.active) on_active_changed(ev.xproperty.time);
            break;
        case MappingNotify:
            XRefreshKeyboardMapping(&ev.xmapping);
            if (ev.xmapping.request == MappingKeyboard) build_keymap();
            break;
        default:
            break;
    }
}

int g_xi_major = 0, g_xi_minor = 0;

// (Re)select raw input events; called again on reload when count-motion changes.
void select_input_events() {
    unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(bits, XI_RawKeyPress);
    XISetMask(bits, XI_RawKeyRelease);
    XISetMask(bits, XI_RawButtonPress);
#ifdef XI_RawTouchBegin
    if (g_xi_major > 2 || g_xi_minor >= 2) XISetMask(bits, XI_RawTouchBegin);
#endif
    if (g_cfg.count_motion) XISetMask(bits, XI_RawMotion);  // only if asked: it's a firehose
    XIEventMask mask{XIAllMasterDevices, (int)sizeof bits, bits};
    XISelectEvents(g_dpy, g_root, &mask, 1);
    XFlush(g_dpy);
}

bool setup_xinput() {
    int event = 0, error = 0;
    if (!XQueryExtension(g_dpy, "XInputExtension", &g_xi_opcode, &event, &error)) return false;
    int major = 2, minor = 2;
    if (XIQueryVersion(g_dpy, &major, &minor) != Success || major < 2) return false;
    if (major == 2 && minor < 1)
        fg::logmsg(0, "warning: XInput %d.%d: input during other apps' grabs is invisible", major, minor);
    g_xi_major = major;
    g_xi_minor = minor;
    select_input_events();

    int n = 0;
    if (XIDeviceInfo* info = XIQueryDevice(g_dpy, XIAllDevices, &n)) {
        for (int i = 0; i < n; ++i)
            if (info[i].name && std::strstr(info[i].name, "XTEST")) g_xtest_devices.insert(info[i].deviceid);
        XIFreeDeviceInfo(info);
    }
    return true;
}

// ------------------------------------------------------------------ process

std::string default_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/focusguard.conf";
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/.config/focusguard.conf" : std::string();
}

// Returns false if another instance holds the lock.
bool acquire_single_instance_lock() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    const std::string path = dir && *dir ? std::string(dir) + "/focusguard.lock"
                                         : "/tmp/focusguard-" + std::to_string(getuid()) + ".lock";
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true;  // can't check; run anyway
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    return true;  // fd intentionally kept open for the process lifetime
}

void on_signal(int sig) {
    char c = 'q';  // SIGINT/SIGTERM
    if (sig == SIGUSR1) c = 'p';       // pause / resume
    else if (sig == SIGUSR2) c = 's';  // print statistics
    else if (sig == SIGHUP) c = 'r';   // reload settings
    const ssize_t n = write(g_sigpipe[1], &c, 1);
    (void)n;
}

// Leave the main loop at its next turn (safe from callbacks and handlers).
void quit_soon() {
    const char c = 'q';
    const ssize_t n = write(g_sigpipe[1], &c, 1);
    (void)n;
}

// Pause for `minutes`, or (0) until resumed; or resume.
void set_paused(bool on, uint32_t minutes = 0) {
    g_resume_at = on && minutes ? fg::wall_ms() + minutes * 60000LL : 0;
    if (on != g_paused) {
        g_paused = on;
        if (!on) {
            g_current = active_window();
            g_expected = 0;
        }
    }
    if (!on) fg::logmsg(1, "resumed");
    else if (minutes) fg::logmsg(1, "paused until %s", fg::local_hhmm(g_resume_at).c_str());
    else fg::logmsg(1, "paused");
    sync_bus(false);
}

void toggle_pause() { set_paused(!g_paused); }

// Bring the window the user asked for (from a notification) to the front.
void show_window(Window w) {
    XWindowAttributes a;
    g_xerror = false;
    if (!XGetWindowAttributes(g_dpy, w, &a) || g_xerror) return;  // closed meanwhile
    g_expected = w;  // our own doing: don't judge it
    send_wm_message(w, A.active, 2, CurrentTime, 0, 0);
    XFlush(g_dpy);
}

// "Always allow" from a notification: add the app to the allow list in the
// settings file, then reload it.
void allow_app(const std::string& entry) {
    const std::string path = g_cfg.config_path.empty() ? default_config_path() : g_cfg.config_path;
    std::string err;
    if (path.empty() || !fg::add_to_config_file(path, true, entry, err)) {
        fg::logmsg(0, "couldn't add '%s' to the allow list: %s", entry.c_str(), err.c_str());
        return;
    }
    fg::logmsg(1, "'%s' added to the allow list in %s", entry.c_str(), path.c_str());
    reload();
}

// ------------------------------------------------------------------ tray

bool strict_mode() { return !(g_cfg.allow_same_app && g_cfg.allow_launched && g_cfg.allow_related); }

// Open the settings file in the default editor, creating it first if needed.
void open_config() {
    const std::string path = g_cfg.config_path.empty() ? default_config_path() : g_cfg.config_path;
    if (path.empty()) return;
    const std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    if (!std::filesystem::exists(p, ec)) {
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            std::fputs("# FocusGuard settings: see 'focusguard --help' or focusguard.conf.example.\n"
                       "# Save, then choose \"Reload settings\" in the tray menu.\n",
                       f);
            std::fclose(f);
        }
    }
    fg::logmsg(1, "opening %s", path.c_str());
    if (fork() != 0) return;
    execlp("xdg-open", "xdg-open", path.c_str(), (char*)nullptr);
    _exit(127);
}

// Open the settings app: next to this executable, then on the PATH; without
// it, fall back to the settings file in a text editor.
void open_settings() {
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    std::string sibling;
    if (n > 0) {
        self[n] = '\0';
        sibling = std::filesystem::path(self).parent_path() / "focusguard-settings";
    }
    const bool have_app = (!sibling.empty() && access(sibling.c_str(), X_OK) == 0);
    if (!have_app) {
        const char* path = std::getenv("PATH");
        bool on_path = false;
        for (std::string dirs = path ? path : ""; !dirs.empty() && !on_path;) {
            const size_t c = dirs.find(':');
            const std::string d = dirs.substr(0, c);
            dirs = c == std::string::npos ? "" : dirs.substr(c + 1);
            on_path = !d.empty() && access((d + "/focusguard-settings").c_str(), X_OK) == 0;
        }
        if (!on_path) {
            open_config();
            return;
        }
        sibling = "focusguard-settings";
    }
    if (fork() != 0) return;
    execlp(sibling.c_str(), sibling.c_str(), (char*)nullptr);
    _exit(127);
}

// A tray menu toggle: kept across reloads as an override (see fg::Overrides).
void flip(fg::Toggle t, const char* what) {
    g_overrides.flip(t, g_file_cfg, g_cfg);
    fg::logmsg(1, "%s %s", what, fg::toggle_value(g_cfg, t) ? "on" : "off");
}

void on_tray_action(fg::TrayAction a) {
    switch (a) {
        case fg::TrayAction::TogglePause:
            toggle_pause();
            return;  // already synced
        case fg::TrayAction::Pause15:
            set_paused(true, 15);
            return;
        case fg::TrayAction::Pause60:
            set_paused(true, 60);
            return;
        case fg::TrayAction::ToggleStrict: flip(fg::Toggle::Strict, "strict mode"); break;
        case fg::TrayAction::ToggleMinimize: flip(fg::Toggle::Minimize, "minimise-on-block"); break;
        case fg::TrayAction::ToggleNotify: flip(fg::Toggle::Notify, "notifications"); break;
        case fg::TrayAction::ToggleDryRun: flip(fg::Toggle::DryRun, "dry run"); break;
        case fg::TrayAction::ShowStats:
            g_stats.dump("");
#ifdef FOCUSGUARD_TRAY
            g_bus.notify("FocusGuard statistics", g_stats.top_line(), "dialog-information");
#endif
            return;
        case fg::TrayAction::EditConfig:
            open_settings();
            return;
        case fg::TrayAction::Reload:
            reload();
            return;
        case fg::TrayAction::Quit:
            quit_soon();
            return;
    }
    sync_bus(false);
}

// Show the XEmbed icon when the tray is on but no StatusNotifierItem host has
// (or is about to have) the icon. Cheap; called after every D-Bus pump.
void sync_xembed() {
#ifdef FOCUSGUARD_TRAY
    const bool sni = g_bus.host() != fg::DesktopBus::Host::Absent;
#else
    const bool sni = false;
#endif
    const bool want = g_cfg.tray && !sni;
    g_xembed.set_style(g_paused ? fg::icon::Style::Paused : g_cfg.dry_run ? fg::icon::Style::DryRun : fg::icon::Style::Active);
    g_xembed.set_wanted(want);
    if (!want || g_xembed.docked()) {
        g_warned_no_tray = false;
    } else if (!g_warned_no_tray && !g_xembed.tray_exists()) {
        g_warned_no_tray = true;
        fg::logmsg(1, "no system tray found; the icon appears once one starts (a StatusNotifierItem tray: "
                      "KDE, GNOME with the AppIndicator extension, Xfce, Cinnamon, LXQt, ...; or an XEmbed "
                      "tray: i3bar, polybar, stalonetray, ...)");
    }
}

// Bring the tray icon (and the bus it needs) in line with the settings.
// Connecting is only attempted at startup and on reload, not on every toggle.
void sync_bus(bool may_connect) {
    push_status();
#ifdef FOCUSGUARD_TRAY
    if (may_connect && (g_cfg.tray || g_cfg.notify) && !g_bus.connected()) g_bus.connect(on_tray_action);
    if (g_bus.connected()) {
        fg::TrayState st;
        st.paused = g_paused;
        st.strict = strict_mode();
        st.minimize = g_cfg.minimize_on_block;
        st.notify = g_cfg.notify;
        st.dry_run = g_cfg.dry_run;
        st.resume_at = g_paused ? g_resume_at : 0;
        g_bus.set_state(st);
        g_bus.set_tray(g_cfg.tray);
    }
#else
    (void)may_connect;
#endif
    sync_xembed();
}

}  // namespace

int main(int argc, char** argv) {
    g_args.assign(argv, argv + argc);
    switch (fg::parse_command_line(g_args, g_cfg, default_config_path())) {
        case fg::ParseResult::ExitOk: return 0;
        case fg::ParseResult::ExitError: return 2;
        case fg::ParseResult::Run: break;
    }
    fg::apply_logging(g_cfg);
    rebuild_matchers();
    g_file_cfg = g_cfg;

    if (!acquire_single_instance_lock()) {
        fg::logmsg(0, "FocusGuard is already running.");
        return 1;
    }

    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) {
        const char* d = std::getenv("DISPLAY");
        fg::logmsg(0, "error: cannot open X display '%s'", d ? d : "(DISPLAY not set)");
        return 1;
    }
    const char* session = std::getenv("XDG_SESSION_TYPE");
    if (session && std::strcmp(session, "wayland") == 0)
        fg::logmsg(0, "warning: Wayland session - only XWayland windows are visible to FocusGuard "
                      "(Wayland compositors already restrict focus stealing themselves)");

    XSetErrorHandler(on_xerror);
    g_root = DefaultRootWindow(g_dpy);

    const char* names[] = {"_NET_ACTIVE_WINDOW", "_NET_WM_PID", "_NET_WM_NAME", "UTF8_STRING",
                           "_NET_WM_STATE", "_NET_WM_STATE_DEMANDS_ATTENTION", "_NET_WM_STATE_HIDDEN",
                           "_NET_CLIENT_LIST", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK",
                           "_NET_WM_WINDOW_TYPE_DESKTOP"};
    Atom atoms[11];
    XInternAtoms(g_dpy, const_cast<char**>(names), 11, False, atoms);
    A = {atoms[0], atoms[1], atoms[2], atoms[3], atoms[4], atoms[5], atoms[6], atoms[7], atoms[8], atoms[9],
         atoms[10]};

    int xkb_op, xkb_ev, xkb_err, xkb_major = XkbMajorVersion, xkb_minor = XkbMinorVersion;
    XkbQueryExtension(g_dpy, &xkb_op, &xkb_ev, &xkb_err, &xkb_major, &xkb_minor);
    build_keymap();

    if (!setup_xinput()) {
        fg::logmsg(0, "error: the X server has no XInput 2 support");
        return 1;
    }
    // StructureNotify: system trays announce themselves to the root window.
    XSelectInput(g_dpy, g_root, PropertyChangeMask | StructureNotifyMask);
    g_xembed.init(g_dpy, on_tray_action);

    bool has_active = false;
    g_current = active_window(&has_active);
    if (!has_active)
        fg::logmsg(0, "warning: the window manager doesn't publish _NET_ACTIVE_WINDOW (yet); "
                      "FocusGuard needs an EWMH-compliant window manager");

    if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        fg::logmsg(0, "error: pipe2 failed");
        return 1;
    }
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (int sig : {SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2}) sigaction(sig, &sa, nullptr);
    // A closed stderr pipe or X socket must not kill us silently; Xlib reports
    // a dead connection through its own error handler.
    signal(SIGPIPE, SIG_IGN);
    // Auto-reap notify-send children without a wait() of our own.
    struct sigaction chld{};
    chld.sa_handler = SIG_DFL;
    chld.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &chld, nullptr);

    if (!g_ctl.start(fg::control_endpoint()))
        fg::logmsg(0, "warning: cannot open the control socket %s; the settings app can't connect",
                   fg::control_endpoint().c_str());
    sync_history();
    sync_bus(true);

    const bool strict = strict_mode();
    fg::logmsg(1, "FocusGuard %s running on %s%s%s - grace %u ms, launch grace %u ms, %u allow, %u block%s%s",
               fg::kVersion, DisplayString(g_dpy), g_cfg.dry_run ? " (DRY RUN)" : "",
               strict ? " (strict)" : "", (unsigned)g_cfg.grace_ms, (unsigned)g_cfg.launch_grace_ms,
               (unsigned)g_allow.size(), (unsigned)g_block.size(),
               g_cfg.config_path.empty() ? "" : ", config ", g_cfg.config_path.c_str());
    fg::logmsg(1, "Ctrl+C quits; USR1 pauses, USR2 prints stats, HUP reloads "
                  "(e.g. 'pkill -USR1 focusguard')%s.",
               g_cfg.tray ? "; or use the tray icon" : "");

    const fg::ControlHooks control_hooks{
        status_json,
        [] { return fg::windows_json(list_windows()); },
        [](bool pause, uint32_t minutes) { set_paused(pause, minutes); },
        reload,
        quit_soon,
        clear_history,
        [] {
            reset_stats();
            push_status();
        },
    };

    const int xfd = ConnectionNumber(g_dpy);
    bool running = true;
    while (running) {
        // Drain both queues until neither has work: handling one can fill the
        // other's client-side buffer (a round trip reads ahead), and poll()
        // only sees what is still in the socket.
        for (bool more = true; more;) {
            while (XPending(g_dpy)) {
                XEvent ev;
                XNextEvent(g_dpy, &ev);
                handle_event(ev);
            }
#ifdef FOCUSGUARD_TRAY
            more = g_bus.pump();
#else
            more = false;
#endif
            sync_xembed();  // a StatusNotifierItem host may have come or gone
        }
#ifdef FOCUSGUARD_TRAY
        const int bus_fd = g_bus.fd();
#else
        const int bus_fd = -1;  // poll() ignores negative fds
#endif
        std::vector<pollfd> fds = {{xfd, POLLIN, 0}, {g_sigpipe[0], POLLIN, 0}, {bus_fd, POLLIN, 0}};
        g_ctl.fill(fds);
        // A timed pause wakes us when it ends. The wall clock is checked on
        // every turn, so time spent suspended counts too.
        int timeout = -1;
        if (g_paused && g_resume_at) {
            const long long left = g_resume_at - fg::wall_ms();
            timeout = left <= 0 ? 0 : (int)std::min(left, 60000LL);
        }
        const int ready = poll(fds.data(), fds.size(), timeout);
        if (g_paused && g_resume_at && fg::wall_ms() >= g_resume_at) {
            fg::logmsg(1, "pause over");
            set_paused(false);
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & POLLIN) {
            char c;
            while (read(g_sigpipe[0], &c, 1) == 1) {
                switch (c) {
                    case 'q': running = false; break;
                    case 'p': toggle_pause(); break;
                    case 's': g_stats.dump(""); break;
                    case 'r': reload(); break;
                    default: break;
                }
            }
        }
        g_ctl.handle(fds, [&](const std::string& line, bool& subscribe) {
            return fg::handle_control(line, g_events, control_hooks, subscribe);
        });
        if (fds[0].revents & (POLLHUP | POLLERR)) {
            fg::logmsg(0, "lost the X connection");
            break;
        }
    }

    g_stats.dump(" this session");
    g_ctl.stop();
#ifdef FOCUSGUARD_TRAY
    g_bus.close();
#endif
    XCloseDisplay(g_dpy);
    fg::logmsg(1, "FocusGuard stopped.");
    return 0;
}
