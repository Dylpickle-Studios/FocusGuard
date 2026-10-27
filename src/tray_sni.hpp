// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard - Linux tray icon and desktop notifications, spoken directly over
// the D-Bus session bus with libdbus.
//
// * The icon is a StatusNotifierItem (the freedesktop/KDE tray protocol),
//   shown by KDE Plasma, GNOME with the AppIndicator extension, Xfce, Cinnamon,
//   LXQt, Budgie, waybar and others. Its menu is exported as com.canonical.dbusmenu.
//   The panel draws both, so FocusGuard needs no GUI toolkit.
// * The icon images come from icon.hpp, so they don't depend on the icon theme.
// * Notifications go straight to org.freedesktop.Notifications.
// * Nothing blocks: the connection's fd joins the main poll() loop, and menu
//   clicks are queued and run after dispatch, outside libdbus callbacks.
#pragma once

#include <dbus/dbus.h>
#include <unistd.h>

#include <cmath>
#include <functional>

#include "common.hpp"
#include "icon.hpp"
#include "tray.hpp"

namespace fg {

namespace sni {

constexpr const char* kItemPath = "/StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";
constexpr const char* kItemIface = "org.kde.StatusNotifierItem";
constexpr const char* kMenuIface = "com.canonical.dbusmenu";
constexpr const char* kWatcher = "org.kde.StatusNotifierWatcher";

// ------------------------------------------------------------------ marshalling

// Thin builder over DBusMessageIter: containers take a lambda that fills them.
struct Out {
    DBusMessageIter it;

    void s(const char* v) { dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &v); }
    void s(const std::string& v) { s(v.c_str()); }
    void o(const char* v) { dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &v); }
    void i(int32_t v) {
        const dbus_int32_t x = v;
        dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &x);
    }
    void u(uint32_t v) {
        const dbus_uint32_t x = v;
        dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &x);
    }
    void b(bool v) {
        const dbus_bool_t x = v;
        dbus_message_iter_append_basic(&it, DBUS_TYPE_BOOLEAN, &x);
    }
    void y(uint8_t v) { dbus_message_iter_append_basic(&it, DBUS_TYPE_BYTE, &v); }
    void bytes(const std::vector<uint8_t>& v) {  // the contents of an "ay"
        const uint8_t* p = v.data();
        dbus_message_iter_append_fixed_array(&it, DBUS_TYPE_BYTE, &p, (int)v.size());
    }

    template <class F> void container(int type, const char* sig, F&& fill) {
        Out sub;
        dbus_message_iter_open_container(&it, type, sig, &sub.it);
        fill(sub);
        dbus_message_iter_close_container(&it, &sub.it);
    }
    template <class F> void variant(const char* sig, F&& f) { container(DBUS_TYPE_VARIANT, sig, f); }
    template <class F> void array(const char* sig, F&& f) { container(DBUS_TYPE_ARRAY, sig, f); }
    template <class F> void strct(F&& f) { container(DBUS_TYPE_STRUCT, nullptr, f); }
    template <class F> void entry(F&& f) { container(DBUS_TYPE_DICT_ENTRY, nullptr, f); }
};

inline auto v_str(const char* s) {
    return [s](Out& o) { o.variant("s", [&](Out& v) { v.s(s); }); };
}
inline auto v_bool(bool b) {
    return [b](Out& o) { o.variant("b", [&](Out& v) { v.b(b); }); };
}
inline auto v_int(int32_t n) {
    return [n](Out& o) { o.variant("i", [&](Out& v) { v.i(n); }); };
}

// ------------------------------------------------------------------ icon

// One image of the icon: ARGB32, network byte order, as IconPixmap wants.
struct Pixmap {
    int32_t size;
    std::vector<uint8_t> argb;
};

inline std::vector<Pixmap> render_icon(icon::Style st) {
    std::vector<Pixmap> out;
    for (int size : {16, 22, 24, 32, 48, 64}) {
        Pixmap pm{size, {}};
        for (uint32_t px : icon::render(st, size))
            for (int shift : {24, 16, 8, 0}) pm.argb.push_back((uint8_t)(px >> shift));
        out.push_back(std::move(pm));
    }
    return out;
}

// ------------------------------------------------------------------ menu

enum class Check { Plain, Paused, Strict, Minimize, Notify, DryRun };

struct MenuItem {
    int32_t id;
    const char* label;  // nullptr: separator
    Check check;
    TrayAction action;
};

// The same entries as the Windows tray menu. Id 0 is the (invisible) root.
inline constexpr MenuItem kMenu[] = {
    {1, "Paused", Check::Paused, TrayAction::TogglePause},
    {2, nullptr, Check::Plain, TrayAction::Quit},
    {3, "Strict mode", Check::Strict, TrayAction::ToggleStrict},
    {4, "Minimise blocked windows", Check::Minimize, TrayAction::ToggleMinimize},
    {5, "Notifications", Check::Notify, TrayAction::ToggleNotify},
    {6, "Dry run (log only)", Check::DryRun, TrayAction::ToggleDryRun},
    {7, nullptr, Check::Plain, TrayAction::Quit},
    {8, "Show statistics", Check::Plain, TrayAction::ShowStats},
    {9, "Settings…", Check::Plain, TrayAction::EditConfig},
    {10, "Reload settings", Check::Plain, TrayAction::Reload},
    {11, nullptr, Check::Plain, TrayAction::Quit},
    {12, "Quit", Check::Plain, TrayAction::Quit},
};

inline const MenuItem* find_item(int32_t id) {
    for (const MenuItem& m : kMenu)
        if (m.id == id) return &m;
    return nullptr;
}

inline bool checked(Check c, const TrayState& s) {
    switch (c) {
        case Check::Paused: return s.paused;
        case Check::Strict: return s.strict;
        case Check::Minimize: return s.minimize;
        case Check::Notify: return s.notify;
        case Check::DryRun: return s.dry_run;
        case Check::Plain: break;
    }
    return false;
}

// Calls emit(name, writer) for each property of a menu item (nullptr = root);
// writer(Out&) appends the value as a variant.
template <class F> void for_each_item_prop(const MenuItem* m, const TrayState& s, F&& emit) {
    if (!m) {
        emit("children-display", v_str("submenu"));
        return;
    }
    if (!m->label) {
        emit("type", v_str("separator"));
        return;
    }
    emit("label", v_str(m->label));
    emit("enabled", v_bool(true));
    emit("visible", v_bool(true));
    if (m->check != Check::Plain) {
        emit("toggle-type", v_str("checkmark"));
        emit("toggle-state", v_int(checked(m->check, s) ? 1 : 0));
    }
}

// ------------------------------------------------------------------ introspection

constexpr const char* kIntrospectHeader =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n";

constexpr const char* kIntrospectCommon =
    " <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "  <method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"v\" direction=\"out\"/></method>\n"
    "  <method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>\n"
    "  <method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"v\" direction=\"in\"/></method>\n"
    "  <signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal>\n"
    " </interface>\n"
    " <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "  <method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method>\n"
    " </interface>\n"
    "</node>\n";

constexpr const char* kIntrospectItem =
    " <interface name=\"org.kde.StatusNotifierItem\">\n"
    "  <property name=\"Category\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Id\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Title\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Status\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"WindowId\" type=\"i\" access=\"read\"/>\n"
    "  <property name=\"IconThemePath\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"IconName\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"IconPixmap\" type=\"a(iiay)\" access=\"read\"/>\n"
    "  <property name=\"OverlayIconName\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"OverlayIconPixmap\" type=\"a(iiay)\" access=\"read\"/>\n"
    "  <property name=\"AttentionIconName\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"AttentionIconPixmap\" type=\"a(iiay)\" access=\"read\"/>\n"
    "  <property name=\"AttentionMovieName\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"ToolTip\" type=\"(sa(iiay)ss)\" access=\"read\"/>\n"
    "  <property name=\"ItemIsMenu\" type=\"b\" access=\"read\"/>\n"
    "  <property name=\"Menu\" type=\"o\" access=\"read\"/>\n"
    "  <method name=\"ContextMenu\"><arg name=\"x\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "  <method name=\"Activate\"><arg name=\"x\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "  <method name=\"SecondaryActivate\"><arg name=\"x\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "  <method name=\"Scroll\"><arg name=\"delta\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"orientation\" type=\"s\" direction=\"in\"/></method>\n"
    "  <signal name=\"NewTitle\"/>\n  <signal name=\"NewIcon\"/>\n  <signal name=\"NewAttentionIcon\"/>\n"
    "  <signal name=\"NewOverlayIcon\"/>\n  <signal name=\"NewToolTip\"/>\n"
    "  <signal name=\"NewStatus\"><arg name=\"status\" type=\"s\"/></signal>\n"
    " </interface>\n";

constexpr const char* kIntrospectMenu =
    " <interface name=\"com.canonical.dbusmenu\">\n"
    "  <property name=\"Version\" type=\"u\" access=\"read\"/>\n"
    "  <property name=\"TextDirection\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Status\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"IconThemePath\" type=\"as\" access=\"read\"/>\n"
    "  <method name=\"GetLayout\"><arg type=\"i\" name=\"parentId\" direction=\"in\"/>"
    "<arg type=\"i\" name=\"recursionDepth\" direction=\"in\"/><arg type=\"as\" name=\"propertyNames\" direction=\"in\"/>"
    "<arg type=\"u\" name=\"revision\" direction=\"out\"/><arg type=\"(ia{sv}av)\" name=\"layout\" direction=\"out\"/></method>\n"
    "  <method name=\"GetGroupProperties\"><arg type=\"ai\" name=\"ids\" direction=\"in\"/>"
    "<arg type=\"as\" name=\"propertyNames\" direction=\"in\"/><arg type=\"a(ia{sv})\" name=\"properties\" direction=\"out\"/></method>\n"
    "  <method name=\"GetProperty\"><arg type=\"i\" name=\"id\" direction=\"in\"/><arg type=\"s\" name=\"name\" direction=\"in\"/>"
    "<arg type=\"v\" name=\"value\" direction=\"out\"/></method>\n"
    "  <method name=\"Event\"><arg type=\"i\" name=\"id\" direction=\"in\"/><arg type=\"s\" name=\"eventId\" direction=\"in\"/>"
    "<arg type=\"v\" name=\"data\" direction=\"in\"/><arg type=\"u\" name=\"timestamp\" direction=\"in\"/></method>\n"
    "  <method name=\"EventGroup\"><arg type=\"a(isvu)\" name=\"events\" direction=\"in\"/>"
    "<arg type=\"ai\" name=\"idErrors\" direction=\"out\"/></method>\n"
    "  <method name=\"AboutToShow\"><arg type=\"i\" name=\"id\" direction=\"in\"/>"
    "<arg type=\"b\" name=\"needUpdate\" direction=\"out\"/></method>\n"
    "  <method name=\"AboutToShowGroup\"><arg type=\"ai\" name=\"ids\" direction=\"in\"/>"
    "<arg type=\"ai\" name=\"updatesNeeded\" direction=\"out\"/><arg type=\"ai\" name=\"idErrors\" direction=\"out\"/></method>\n"
    "  <signal name=\"ItemsPropertiesUpdated\"><arg type=\"a(ia{sv})\" name=\"updatedProps\"/>"
    "<arg type=\"a(ias)\" name=\"removedProps\"/></signal>\n"
    "  <signal name=\"LayoutUpdated\"><arg type=\"u\" name=\"revision\"/><arg type=\"i\" name=\"parent\"/></signal>\n"
    "  <signal name=\"ItemActivationRequested\"><arg type=\"i\" name=\"id\"/><arg type=\"u\" name=\"timestamp\"/></signal>\n"
    " </interface>\n";

// Method call check that also accepts calls without an interface.
inline bool is_call(DBusMessage* m, const char* iface, const char* member) {
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return false;
    const char* mi = dbus_message_get_interface(m);
    const char* mm = dbus_message_get_member(m);
    return mm && std::strcmp(mm, member) == 0 && (!mi || !iface || std::strcmp(mi, iface) == 0);
}

}  // namespace sni

// ------------------------------------------------------------------ the bus

class DesktopBus {
public:
    using Handler = std::function<void(TrayAction)>;

    ~DesktopBus() { close(); }

    // Connect to the session bus. Logs and returns false if there is none.
    bool connect(Handler h) {
        if (conn_) return true;
        handler_ = std::move(h);
        DBusError err;
        dbus_error_init(&err);
        conn_ = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
        if (!conn_) {
            logmsg(0, "warning: no D-Bus session bus (%s): no tray icon or notifications",
                   err.message ? err.message : "unknown error");
            dbus_error_free(&err);
            return false;
        }
        dbus_connection_set_exit_on_disconnect(conn_, FALSE);  // losing the bus must not kill us

        static DBusObjectPathVTable item_vt{}, menu_vt{};
        item_vt.message_function = [](DBusConnection*, DBusMessage* m, void* self) {
            return static_cast<DesktopBus*>(self)->on_item(m);
        };
        menu_vt.message_function = [](DBusConnection*, DBusMessage* m, void* self) {
            return static_cast<DesktopBus*>(self)->on_menu(m);
        };
        dbus_connection_register_object_path(conn_, sni::kItemPath, &item_vt, this);
        dbus_connection_register_object_path(conn_, sni::kMenuPath, &menu_vt, this);
        dbus_connection_add_filter(
            conn_, [](DBusConnection*, DBusMessage* m, void* self) { return static_cast<DesktopBus*>(self)->on_signal(m); },
            this, nullptr);
        // Re-register whenever a panel (the StatusNotifierWatcher) starts or restarts.
        dbus_bus_add_match(conn_,
                           "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                           "member='NameOwnerChanged',arg0='org.kde.StatusNotifierWatcher'",
                           nullptr);
        query_notify_caps();
        dbus_connection_flush(conn_);
        return true;
    }

    bool connected() const { return conn_ != nullptr; }

    // Whether a StatusNotifierItem host shows the icon. While it is Absent, the
    // daemon may fall back to an XEmbed tray icon instead.
    enum class Host { Absent, Pending, Registered };
    Host host() const { return conn_ && tray_on_ ? host_ : Host::Absent; }

    int fd() const {
        int fd = -1;
        if (conn_ && !dbus_connection_get_unix_fd(conn_, &fd)) fd = -1;
        return fd;
    }

    // Show or hide the tray icon. Hiding releases the bus name, which is how a
    // panel learns that an item went away.
    void set_tray(bool on) {
        if (!conn_ || on == tray_on_) return;
        if (on) {
            name_ = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
            DBusError err;
            dbus_error_init(&err);
            const int r = dbus_bus_request_name(conn_, name_.c_str(), DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
            if (r != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
                logmsg(0, "warning: tray icon: cannot own %s (%s)", name_.c_str(),
                       dbus_error_is_set(&err) ? err.message : "name taken");
                dbus_error_free(&err);
                return;
            }
            tray_on_ = true;
            register_item();
        } else {
            dbus_bus_release_name(conn_, name_.c_str(), nullptr);
            tray_on_ = false;
        }
        dbus_connection_flush(conn_);
    }

    void set_state(const TrayState& s) {
        if (have_state_ && s == state_) return;
        const bool icon_changed = !have_state_ || s.paused != state_.paused || s.dry_run != state_.dry_run;
        state_ = s;
        have_state_ = true;
        ++revision_;
        if (!conn_ || !tray_on_) return;
        if (icon_changed) {
            emit(sni::kItemPath, sni::kItemIface, "NewIcon");
            emit(sni::kItemPath, sni::kItemIface, "NewToolTip");
        }
        DBusMessage* m = dbus_message_new_signal(sni::kMenuPath, sni::kMenuIface, "LayoutUpdated");
        sni::Out o;
        dbus_message_iter_init_append(m, &o.it);
        o.u(revision_);
        o.i(0);
        send(m);
    }

    // Fire-and-forget desktop notification.
    void notify(const std::string& summary, const std::string& body, const char* icon) {
        if (!conn_) return;
        if (!caps_known_) query_notify_caps();
        DBusMessage* m = dbus_message_new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                                      "org.freedesktop.Notifications", "Notify");
        dbus_message_set_no_reply(m, TRUE);
        sni::Out o;
        dbus_message_iter_init_append(m, &o.it);
        o.s("FocusGuard");
        o.u(0);  // replaces_id
        o.s(icon);
        o.s(summary);
        o.s(markup_ ? escape_markup(body) : body);
        o.array("s", [](sni::Out&) {});  // no actions
        o.array("{sv}", [](sni::Out& a) {
            a.entry([](sni::Out& e) {
                e.s("urgency");
                e.variant("y", [](sni::Out& v) { v.y(0); });  // low
            });
        });
        o.i(-1);  // server's default timeout
        send(m);
    }

    // Handle incoming traffic and run queued menu actions. Call it before
    // poll() and whenever fd() is readable. Returns true if it did anything,
    // in which case the caller should drain its other event sources again.
    bool pump() {
        if (!conn_) return false;
        bool worked = false;
        dbus_connection_read_write(conn_, 0);
        while (dbus_connection_get_dispatch_status(conn_) == DBUS_DISPATCH_DATA_REMAINS) {
            dbus_connection_dispatch(conn_);
            worked = true;
        }
        dbus_connection_flush(conn_);
        if (!dbus_connection_get_is_connected(conn_)) {
            logmsg(0, "lost the D-Bus session bus: tray icon and notifications disabled");
            close();
            return true;
        }
        std::vector<TrayAction> actions;
        actions.swap(pending_);
        for (TrayAction a : actions) handler_(a);
        return worked || !actions.empty();
    }

    void close() {
        if (!conn_) return;
        dbus_connection_close(conn_);
        dbus_connection_unref(conn_);
        conn_ = nullptr;
        tray_on_ = false;
    }

private:
    // ---- outgoing

    void send(DBusMessage* m) {
        dbus_connection_send(conn_, m, nullptr);
        dbus_message_unref(m);
        dbus_connection_flush(conn_);
    }

    void emit(const char* path, const char* iface, const char* name) {
        send(dbus_message_new_signal(path, iface, name));
    }

    // Async method call; on_reply(reply) runs during a later dispatch.
    template <class F> void call_async(DBusMessage* m, F on_reply) {
        DBusPendingCall* pending = nullptr;
        if (!dbus_connection_send_with_reply(conn_, m, &pending, 5000) || !pending) {
            dbus_message_unref(m);
            return;
        }
        dbus_message_unref(m);
        auto* fn = new F(std::move(on_reply));
        dbus_pending_call_set_notify(
            pending,
            [](DBusPendingCall* p, void* data) {
                if (DBusMessage* r = dbus_pending_call_steal_reply(p)) {
                    (*static_cast<F*>(data))(r);
                    dbus_message_unref(r);
                }
            },
            fn, [](void* data) { delete static_cast<F*>(data); });
        dbus_pending_call_unref(pending);  // the connection keeps its own reference
    }

    void register_item() {
        host_ = Host::Pending;
        DBusMessage* m = dbus_message_new_method_call(sni::kWatcher, "/StatusNotifierWatcher", sni::kWatcher,
                                                      "RegisterStatusNotifierItem");
        sni::Out o;
        dbus_message_iter_init_append(m, &o.it);
        o.s(name_);
        call_async(m, [this](DBusMessage* r) {
            if (dbus_message_get_type(r) != DBUS_MESSAGE_TYPE_ERROR) {
                host_ = Host::Registered;
                logmsg(2, "tray icon registered with the panel (StatusNotifierItem)");
            } else {
                host_ = Host::Absent;
                logmsg(2, "no StatusNotifierItem host");
            }
        });
    }

    void query_notify_caps() {
        DBusMessage* m = dbus_message_new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                                      "org.freedesktop.Notifications", "GetCapabilities");
        call_async(m, [this](DBusMessage* r) {
            DBusMessageIter it, arr;
            if (dbus_message_get_type(r) != DBUS_MESSAGE_TYPE_METHOD_RETURN || !dbus_message_iter_init(r, &it) ||
                dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
                return;
            caps_known_ = true;
            markup_ = false;
            for (dbus_message_iter_recurse(&it, &arr); dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING;
                 dbus_message_iter_next(&arr)) {
                const char* cap = nullptr;
                dbus_message_iter_get_basic(&arr, &cap);
                if (cap && std::strcmp(cap, "body-markup") == 0) markup_ = true;
            }
        });
    }

    static std::string escape_markup(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '&') out += "&amp;";
            else if (c == '<') out += "&lt;";
            else if (c == '>') out += "&gt;";
            else out += c;
        }
        return out;
    }

    template <class F> void reply(DBusMessage* m, F&& fill) {
        DBusMessage* r = dbus_message_new_method_return(m);
        sni::Out o;
        dbus_message_iter_init_append(r, &o.it);
        fill(o);
        dbus_connection_send(conn_, r, nullptr);
        dbus_message_unref(r);
    }
    void reply_empty(DBusMessage* m) {
        reply(m, [](sni::Out&) {});
    }
    void reply_error(DBusMessage* m, const char* name, const char* text) {
        DBusMessage* r = dbus_message_new_error(m, name, text);
        dbus_connection_send(conn_, r, nullptr);
        dbus_message_unref(r);
    }

    // ---- StatusNotifierItem

    const std::vector<sni::Pixmap>& icon() {
        const icon::Style st = state_.paused    ? icon::Style::Paused
                               : state_.dry_run ? icon::Style::DryRun
                                                : icon::Style::Active;
        std::vector<sni::Pixmap>& cache = icons_[(int)st];
        if (cache.empty()) cache = sni::render_icon(st);
        return cache;
    }

    const char* tooltip() const {
        return state_.paused    ? "Paused - click to resume"
               : state_.dry_run ? "Dry run - logging only, blocking nothing"
                                : "Protecting your keyboard focus";
    }

    // Appends the value of property `name` as a variant; false if unknown.
    bool item_prop(const char* name, sni::Out& o) {
        auto str = [&](const char* v) { o.variant("s", [&](sni::Out& x) { x.s(v); }); };
        auto pixmaps = [&](const std::vector<sni::Pixmap>* pms) {
            o.variant("a(iiay)", [&](sni::Out& v) {
                v.array("(iiay)", [&](sni::Out& a) {
                    if (!pms) return;
                    for (const sni::Pixmap& p : *pms) {
                        a.strct([&](sni::Out& s) {
                            s.i(p.size);
                            s.i(p.size);
                            s.array("y", [&](sni::Out& y) { y.bytes(p.argb); });
                        });
                    }
                });
            });
        };
        const std::string n = name;
        if (n == "Category") str("ApplicationStatus");
        else if (n == "Id") str("focusguard");
        else if (n == "Title") str("FocusGuard");
        else if (n == "Status") str("Active");
        else if (n == "WindowId") o.variant("i", [](sni::Out& x) { x.i(0); });
        else if (n == "IconThemePath" || n == "IconName" || n == "OverlayIconName" || n == "AttentionIconName" ||
                 n == "AttentionMovieName")
            str("");
        else if (n == "IconPixmap") pixmaps(&icon());
        else if (n == "OverlayIconPixmap" || n == "AttentionIconPixmap") pixmaps(nullptr);
        else if (n == "ToolTip") {
            o.variant("(sa(iiay)ss)", [&](sni::Out& v) {
                v.strct([&](sni::Out& s) {
                    s.s("");
                    s.array("(iiay)", [](sni::Out&) {});
                    s.s("FocusGuard");
                    s.s(tooltip());
                });
            });
        } else if (n == "ItemIsMenu") o.variant("b", [](sni::Out& x) { x.b(false); });
        else if (n == "Menu") o.variant("o", [](sni::Out& x) { x.o(sni::kMenuPath); });
        else return false;
        return true;
    }

    DBusHandlerResult on_item(DBusMessage* m) {
        static const char* const kProps[] = {
            "Category", "Id", "Title", "Status", "WindowId", "IconThemePath", "IconName", "IconPixmap",
            "OverlayIconName", "OverlayIconPixmap", "AttentionIconName", "AttentionIconPixmap",
            "AttentionMovieName", "ToolTip", "ItemIsMenu", "Menu"};
        if (sni::is_call(m, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
            const std::string xml = std::string(sni::kIntrospectHeader) + sni::kIntrospectItem + sni::kIntrospectCommon;
            reply(m, [&](sni::Out& o) { o.s(xml); });
        } else if (sni::is_call(m, DBUS_INTERFACE_PROPERTIES, "Get")) {
            const char *iface = nullptr, *name = nullptr;
            if (!dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
                reply_error(m, DBUS_ERROR_INVALID_ARGS, "expected (ss)");
            } else {
                bool known = false;
                for (const char* p : kProps) known = known || std::strcmp(p, name) == 0;
                if (known) reply(m, [&](sni::Out& o) { item_prop(name, o); });
                else reply_error(m, "org.freedesktop.DBus.Error.UnknownProperty", name);
            }
        } else if (sni::is_call(m, DBUS_INTERFACE_PROPERTIES, "GetAll")) {
            reply(m, [&](sni::Out& o) {
                o.array("{sv}", [&](sni::Out& a) {
                    for (const char* p : kProps)
                        a.entry([&](sni::Out& e) {
                            e.s(p);
                            item_prop(p, e);
                        });
                });
            });
        } else if (sni::is_call(m, DBUS_INTERFACE_PROPERTIES, "Set")) {
            reply_error(m, "org.freedesktop.DBus.Error.PropertyReadOnly", "read-only");
        } else if (sni::is_call(m, nullptr, "Activate")) {  // left click, as on Windows
            pending_.push_back(TrayAction::TogglePause);
            reply_empty(m);
        } else if (sni::is_call(m, nullptr, "ContextMenu") || sni::is_call(m, nullptr, "SecondaryActivate") ||
                   sni::is_call(m, nullptr, "Scroll") || sni::is_call(m, nullptr, "ProvideXdgActivationToken")) {
            reply_empty(m);  // the panel shows the menu itself
        } else {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // ---- dbusmenu

    void write_props(sni::Out& dict_parent, const sni::MenuItem* item) {
        dict_parent.array("{sv}", [&](sni::Out& a) {
            sni::for_each_item_prop(item, state_, [&](const char* name, auto&& value) {
                a.entry([&](sni::Out& e) {
                    e.s(name);
                    value(e);
                });
            });
        });
    }

    // One (ia{sv}av) layout node; the root carries every item as its children.
    void write_layout(sni::Out& st, const sni::MenuItem* item, bool children) {
        st.i(item ? item->id : 0);
        write_props(st, item);
        st.array("v", [&](sni::Out& av) {
            if (item || !children) return;
            for (const sni::MenuItem& child : sni::kMenu)
                av.variant("(ia{sv}av)", [&](sni::Out& v) { v.strct([&](sni::Out& s) { write_layout(s, &child, false); }); });
        });
    }

    void menu_clicked(int32_t id, const char* event) {
        const sni::MenuItem* item = sni::find_item(id);
        if (item && item->label && event && std::strcmp(event, "clicked") == 0) pending_.push_back(item->action);
    }

    static std::vector<int32_t> read_int_array(DBusMessageIter* it) {
        std::vector<int32_t> out;
        if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(it) != DBUS_TYPE_INT32)
            return out;
        DBusMessageIter arr;
        dbus_message_iter_recurse(it, &arr);
        dbus_int32_t* ids = nullptr;
        int n = 0;
        dbus_message_iter_get_fixed_array(&arr, &ids, &n);
        out.assign(ids, ids + n);
        return out;
    }

    DBusHandlerResult on_menu(DBusMessage* m) {
        if (sni::is_call(m, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
            const std::string xml = std::string(sni::kIntrospectHeader) + sni::kIntrospectMenu + sni::kIntrospectCommon;
            reply(m, [&](sni::Out& o) { o.s(xml); });
        } else if (sni::is_call(m, DBUS_INTERFACE_PROPERTIES, "Get") ||
                   sni::is_call(m, DBUS_INTERFACE_PROPERTIES, "GetAll")) {
            const bool all = std::strcmp(dbus_message_get_member(m), "GetAll") == 0;
            const char *iface = nullptr, *name = "";
            if (!all) dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
            auto value = [&](const std::string& n, sni::Out& o) {
                if (n == "Version") o.variant("u", [](sni::Out& v) { v.u(3); });
                else if (n == "TextDirection") o.variant("s", [](sni::Out& v) { v.s("ltr"); });
                else if (n == "Status") o.variant("s", [](sni::Out& v) { v.s("normal"); });
                else if (n == "IconThemePath") o.variant("as", [](sni::Out& v) { v.array("s", [](sni::Out&) {}); });
                else return false;
                return true;
            };
            if (all) {
                reply(m, [&](sni::Out& o) {
                    o.array("{sv}", [&](sni::Out& a) {
                        for (const char* n : {"Version", "TextDirection", "Status", "IconThemePath"})
                            a.entry([&](sni::Out& e) {
                                e.s(n);
                                value(n, e);
                            });
                    });
                });
            } else if (name && (std::string(name) == "Version" || std::string(name) == "TextDirection" ||
                                std::string(name) == "Status" || std::string(name) == "IconThemePath")) {
                reply(m, [&](sni::Out& o) { value(name, o); });
            } else {
                reply_error(m, "org.freedesktop.DBus.Error.UnknownProperty", name ? name : "");
            }
        } else if (sni::is_call(m, sni::kMenuIface, "GetLayout")) {
            dbus_int32_t parent = 0, depth = -1;
            dbus_message_get_args(m, nullptr, DBUS_TYPE_INT32, &parent, DBUS_TYPE_INT32, &depth, DBUS_TYPE_INVALID);
            const sni::MenuItem* item = parent ? sni::find_item(parent) : nullptr;
            if (parent && !item) {
                reply_error(m, DBUS_ERROR_INVALID_ARGS, "no such menu item");
            } else {
                reply(m, [&](sni::Out& o) {
                    o.u(revision_);
                    o.strct([&](sni::Out& s) { write_layout(s, item, depth != 0); });
                });
            }
        } else if (sni::is_call(m, sni::kMenuIface, "GetGroupProperties")) {
            DBusMessageIter it;
            std::vector<int32_t> ids;
            if (dbus_message_iter_init(m, &it)) ids = read_int_array(&it);
            if (ids.empty()) {  // empty list: every item
                ids.push_back(0);
                for (const sni::MenuItem& mi : sni::kMenu) ids.push_back(mi.id);
            }
            reply(m, [&](sni::Out& o) {
                o.array("(ia{sv})", [&](sni::Out& a) {
                    for (int32_t id : ids) {
                        const sni::MenuItem* item = id ? sni::find_item(id) : nullptr;
                        if (id && !item) continue;
                        a.strct([&](sni::Out& s) {
                            s.i(id);
                            write_props(s, item);
                        });
                    }
                });
            });
        } else if (sni::is_call(m, sni::kMenuIface, "GetProperty")) {
            dbus_int32_t id = 0;
            const char* name = nullptr;
            dbus_message_get_args(m, nullptr, DBUS_TYPE_INT32, &id, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
            const sni::MenuItem* item = id ? sni::find_item(id) : nullptr;
            bool found = false;
            if (name && (!id || item)) {
                DBusMessage* r = dbus_message_new_method_return(m);
                sni::Out o;
                dbus_message_iter_init_append(r, &o.it);
                sni::for_each_item_prop(item, state_, [&](const char* n, auto&& value) {
                    if (!found && std::strcmp(n, name) == 0) {
                        value(o);
                        found = true;
                    }
                });
                if (found) dbus_connection_send(conn_, r, nullptr);
                dbus_message_unref(r);
            }
            if (!found) reply_error(m, DBUS_ERROR_INVALID_ARGS, "no such menu item or property");
        } else if (sni::is_call(m, sni::kMenuIface, "Event")) {
            dbus_int32_t id = 0;
            const char* event = nullptr;
            dbus_message_get_args(m, nullptr, DBUS_TYPE_INT32, &id, DBUS_TYPE_STRING, &event, DBUS_TYPE_INVALID);
            menu_clicked(id, event);
            reply_empty(m);
        } else if (sni::is_call(m, sni::kMenuIface, "EventGroup")) {
            DBusMessageIter it, arr, st;
            if (dbus_message_iter_init(m, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                for (dbus_message_iter_recurse(&it, &arr); dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT;
                     dbus_message_iter_next(&arr)) {
                    dbus_message_iter_recurse(&arr, &st);
                    dbus_int32_t id = 0;
                    const char* event = nullptr;
                    if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32) continue;
                    dbus_message_iter_get_basic(&st, &id);
                    if (!dbus_message_iter_next(&st) || dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_STRING) continue;
                    dbus_message_iter_get_basic(&st, &event);
                    menu_clicked(id, event);
                }
            }
            reply(m, [](sni::Out& o) { o.array("i", [](sni::Out&) {}); });
        } else if (sni::is_call(m, sni::kMenuIface, "AboutToShow")) {
            reply(m, [](sni::Out& o) { o.b(false); });
        } else if (sni::is_call(m, sni::kMenuIface, "AboutToShowGroup")) {
            reply(m, [](sni::Out& o) {
                o.array("i", [](sni::Out&) {});
                o.array("i", [](sni::Out&) {});
            });
        } else {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // ---- signals from the bus

    DBusHandlerResult on_signal(DBusMessage* m) {
        if (dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
            const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
            if (dbus_message_get_args(m, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
                                      DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
                std::strcmp(name, sni::kWatcher) == 0 && tray_on_) {
                if (*new_owner) {
                    logmsg(2, "tray host appeared; registering the icon");
                    register_item();
                } else {
                    host_ = Host::Absent;  // the panel went away
                }
            }
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusConnection* conn_ = nullptr;
    Handler handler_;
    std::vector<TrayAction> pending_;  // menu clicks, run by pump() after dispatch
    std::string name_;                 // our org.kde.StatusNotifierItem-PID-1 bus name
    bool tray_on_ = false;
    Host host_ = Host::Absent;
    bool caps_known_ = false, markup_ = false;
    TrayState state_;
    bool have_state_ = false;
    uint32_t revision_ = 1;
    std::array<std::vector<sni::Pixmap>, 3> icons_;
};

}  // namespace fg
