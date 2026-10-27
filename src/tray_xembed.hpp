// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard - Linux tray icon for XEmbed system trays (the freedesktop
// "System Tray Protocol": i3bar, polybar, stalonetray, trayer, older panels).
//
// Plain Xlib, no extra dependencies. The icon is a small window docked into
// the tray: left-click pauses / resumes, right-click opens the settings app
// (which has everything the StatusNotifierItem menu has). When the tray
// offers a 32-bit visual the icon has smooth edges; otherwise it is drawn
// with a 1-bit mask over the panel background.
#pragma once

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "icon.hpp"
#include "tray.hpp"

namespace fg {

class XEmbedTray {
public:
    using Handler = std::function<void(TrayAction)>;

    // Call once the display is open. The caller must select StructureNotifyMask
    // on the root window, where trays announce themselves.
    void init(Display* dpy, Handler h) {
        dpy_ = dpy;
        handler_ = std::move(h);
        const int screen = DefaultScreen(dpy);
        root_ = RootWindow(dpy, screen);
        const std::string sel = "_NET_SYSTEM_TRAY_S" + std::to_string(screen);
        sel_ = XInternAtom(dpy, sel.c_str(), False);
        opcode_ = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
        manager_ = XInternAtom(dpy, "MANAGER", False);
        tray_visual_ = XInternAtom(dpy, "_NET_SYSTEM_TRAY_VISUAL", False);
        xembed_info_ = XInternAtom(dpy, "_XEMBED_INFO", False);
    }

    // Whether the icon should be shown. Docks as soon as a tray exists.
    void set_wanted(bool on) {
        if (on == wanted_) return;
        wanted_ = on;
        on ? dock() : undock();
    }

    void set_style(icon::Style s) {
        if (s == style_) return;
        style_ = s;
        redraw();
    }

    bool docked() const { return win_ != 0; }
    bool tray_exists() const { return dpy_ && XGetSelectionOwner(dpy_, sel_) != None; }

    // Handles the events that concern the tray icon; returns true if it did.
    bool handle(const XEvent& ev) {
        if (!dpy_) return false;
        switch (ev.type) {
            case ClientMessage:  // a tray started (or restarted)
                if (ev.xclient.window == root_ && ev.xclient.message_type == manager_ &&
                    (Atom)ev.xclient.data.l[1] == sel_) {
                    if (wanted_ && !win_) dock();
                    return true;
                }
                return false;
            case DestroyNotify:
                if (mgr_ && ev.xdestroywindow.window == mgr_) {  // the tray went away
                    mgr_ = 0;
                    destroy_window();
                    return true;
                }
                if (win_ && ev.xdestroywindow.window == win_) {
                    win_ = 0;
                    return true;
                }
                return false;
            default:
                break;
        }
        if (!win_ || ev.xany.window != win_) return false;
        if (ev.type == Expose && ev.xexpose.count == 0) redraw();
        else if (ev.type == ConfigureNotify) redraw();
        else if (ev.type == ButtonPress && ev.xbutton.button == Button1) handler_(TrayAction::TogglePause);
        else if (ev.type == ButtonPress && ev.xbutton.button == Button3) handler_(TrayAction::EditConfig);
        return true;
    }

private:
    void dock() {
        if (win_ || !dpy_) return;
        mgr_ = XGetSelectionOwner(dpy_, sel_);
        if (!mgr_) return;  // no tray yet: dock when its MANAGER message arrives
        XSelectInput(dpy_, mgr_, StructureNotifyMask);

        // A 32-bit visual offered by the tray gives real transparency.
        Visual* visual = DefaultVisual(dpy_, DefaultScreen(dpy_));
        int depth = DefaultDepth(dpy_, DefaultScreen(dpy_));
        argb_ = false;
        Atom type = None;
        int format = 0;
        unsigned long n = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy_, mgr_, tray_visual_, 0, 1, False, XA_VISUALID, &type, &format, &n, &after,
                               &data) == Success &&
            data && n == 1 && format == 32) {
            XVisualInfo tmpl{};
            tmpl.visualid = (VisualID) reinterpret_cast<unsigned long*>(data)[0];
            int count = 0;
            if (XVisualInfo* vi = XGetVisualInfo(dpy_, VisualIDMask, &tmpl, &count)) {
                if (count > 0 && vi[0].depth == 32) {
                    visual = vi[0].visual;
                    depth = 32;
                    argb_ = true;
                }
                XFree(vi);
            }
        }
        if (data) XFree(data);

        XSetWindowAttributes a{};
        unsigned long mask = CWEventMask | CWBorderPixel | CWColormap;
        a.event_mask = ExposureMask | StructureNotifyMask | ButtonPressMask;
        a.border_pixel = 0;
        a.colormap = argb_ ? XCreateColormap(dpy_, root_, visual, AllocNone)
                           : DefaultColormap(dpy_, DefaultScreen(dpy_));
        if (argb_) {
            a.background_pixel = 0;  // fully transparent
            mask |= CWBackPixel;
        } else {
            a.background_pixmap = ParentRelative;  // show the panel behind the icon
            mask |= CWBackPixmap;
        }
        visual_ = visual;
        depth_ = depth;
        win_ = XCreateWindow(dpy_, root_, 0, 0, 24, 24, 0, depth, InputOutput, visual, mask, &a);
        XStoreName(dpy_, win_, "FocusGuard");
        XClassHint hint{const_cast<char*>("focusguard"), const_cast<char*>("FocusGuard")};
        XSetClassHint(dpy_, win_, &hint);
        const long info[2] = {0, 1};  // XEmbed version 0, XEMBED_MAPPED
        XChangeProperty(dpy_, win_, xembed_info_, xembed_info_, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(info), 2);

        XEvent e{};
        e.xclient.type = ClientMessage;
        e.xclient.window = mgr_;
        e.xclient.message_type = opcode_;
        e.xclient.format = 32;
        e.xclient.data.l[0] = CurrentTime;
        e.xclient.data.l[1] = 0;  // SYSTEM_TRAY_REQUEST_DOCK
        e.xclient.data.l[2] = (long)win_;
        XSendEvent(dpy_, mgr_, False, NoEventMask, &e);
        XFlush(dpy_);
    }

    void undock() {
        destroy_window();
        if (mgr_) XSelectInput(dpy_, mgr_, NoEventMask);
        mgr_ = 0;
    }

    void destroy_window() {
        if (!win_) return;
        XDestroyWindow(dpy_, win_);  // the tray notices and drops the icon
        if (argb_) XFreeColormap(dpy_, colormap());
        win_ = 0;
        XFlush(dpy_);
    }

    Colormap colormap() {
        XWindowAttributes wa;
        return XGetWindowAttributes(dpy_, win_, &wa) ? wa.colormap : 0;
    }

    void redraw() {
        if (!win_) return;
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy_, win_, &wa) || wa.map_state != IsViewable) return;
        const int size = std::min(wa.width, wa.height) - (std::min(wa.width, wa.height) >= 20 ? 2 : 0);
        if (size <= 0) return;
        const int x = (wa.width - size) / 2, y = (wa.height - size) / 2;
        const std::vector<uint32_t> px = icon::render(style_, size);

        // XDestroyImage frees the data with free(), so it must come from malloc.
        auto* data = static_cast<uint32_t*>(std::malloc(px.size() * sizeof(uint32_t)));
        if (!data) return;
        for (size_t i = 0; i < px.size(); ++i) {
            const uint32_t a = px[i] >> 24;
            auto pre = [&](int shift) { return (((px[i] >> shift) & 0xFF) * a / 255) << shift; };
            data[i] = argb_ ? (a << 24 | pre(16) | pre(8) | pre(0))  // premultiplied ARGB
                            : (px[i] & 0x00FFFFFF);                 // colour only; the mask cuts the shape
        }
        XImage* img = XCreateImage(dpy_, visual_, depth_, ZPixmap, 0, reinterpret_cast<char*>(data), size, size,
                                   32, 0);
        if (!img) {
            std::free(data);
            return;
        }
        GC gc = XCreateGC(dpy_, win_, 0, nullptr);
        XClearWindow(dpy_, win_);
        Pixmap mask = None;
        if (!argb_) {
            const int stride = (size + 7) / 8;
            std::string bits((size_t)stride * size, '\0');
            for (int r = 0; r < size; ++r)
                for (int c = 0; c < size; ++c)
                    if ((px[(size_t)r * size + c] >> 24) >= 128) bits[(size_t)r * stride + c / 8] |= (char)(1 << (c % 8));
            mask = XCreateBitmapFromData(dpy_, win_, bits.data(), size, size);
            XSetClipMask(dpy_, gc, mask);
            XSetClipOrigin(dpy_, gc, x, y);
        }
        XPutImage(dpy_, win_, gc, img, 0, 0, x, y, size, size);
        if (mask != None) XFreePixmap(dpy_, mask);
        XFreeGC(dpy_, gc);
        XDestroyImage(img);
        XFlush(dpy_);
    }

    Display* dpy_ = nullptr;
    Handler handler_;
    Window root_ = 0, mgr_ = 0, win_ = 0;
    Atom sel_ = None, opcode_ = None, manager_ = None, tray_visual_ = None, xembed_info_ = None;
    Visual* visual_ = nullptr;
    int depth_ = 0;
    bool argb_ = false, wanted_ = false;
    icon::Style style_ = icon::Style::Active;
};

}  // namespace fg
