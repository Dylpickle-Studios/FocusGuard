# Next additions

What could come after 1.0.1, roughly in order of value. Each item says why it matters, how it could be built, and what to watch out for.

## 1. Wayland support

**Why:** Linux desktops are moving to Wayland: GNOME and KDE Plasma 6 default to it, and Pop!_OS's COSMIC desktop only runs on it. There FocusGuard sees only XWayland windows (apps still using X11), can't tell when a native Wayland window gets focus, and can't hand focus back to one.

**Why it's hard:** Wayland deliberately has no global equivalent of `_NET_ACTIVE_WINDOW` or XInput2 raw events. Only the compositor knows which window is active and what input happened, so FocusGuard needs a small piece *inside* each compositor family. Compositors also already limit focus stealing through `xdg-activation`, so the first step on each one is to measure what still gets through.

**Plan:**

1. **Split the daemon into core and backends.** The decision logic (`common.hpp`), history, control channel and tray stay shared. A backend only reports "window X became active", "the user did something deliberate", basic window info (app id, title, pid), and can "activate window X". The Windows and X11 backends already work this way internally; making it explicit means a Wayland backend can be added without touching the rest.
2. **KDE Plasma (KWin): a KWin script.** It's JavaScript, installed per user (`kpackagetool6`), and gets `workspace.windowActivated` and window properties. It can restore focus by setting `workspace.activeWindow`. It reports to the daemon over D-Bus (`callDBus`). KWin's own focus-stealing prevention levels are a complement; the script handles what they let through.
3. **GNOME (Mutter): a GNOME Shell extension.** `global.display` signals `notify::focus-window`, and `Main.activateWindow(prev)` restores focus. It talks to the daemon over D-Bus. Extensions break with GNOME releases and need review to be listed on extensions.gnome.org, so budget for one update per GNOME version.
4. **wlroots-family compositors through their IPC, no plugin needed:**
   - **Sway:** i3-compatible IPC, with `window` events (`change: focus`) and the `[con_id=N] focus` command.
   - **Hyprland:** `socket2` with `activewindow` events, and `hyprctl dispatch focuswindow address:0x…`.
   - **river** and others: `wlr-foreign-toplevel-management` can observe and activate toplevels where the compositor offers it.
5. **COSMIC:** there's no plugin API yet. Its `zcosmic_toplevel_info_v1` and `zcosmic_toplevel_manager_v1` protocols can list, observe and activate windows, which may be enough for a client-side backend. Follow cosmic-comp's development.

**Input intent** (knowing a change came from your click or Alt+Tab) is the other half. The compositor-side pieces (KWin script, Shell extension) can see input; IPC-only backends can't. For those, either trust the compositor's own activation rules or treat any focus change within the grace period after a *focus-changing* event as user-caused. Avoid reading `/dev/input` directly: it needs extra permissions and sees every keystroke.

**Order:** KDE first (widest use among people who care about this, stable scripting API), then GNOME, then Sway/Hyprland (small and easy once the backend split exists), then COSMIC once its protocols settle.

## 2. Notification buttons on Windows

**Why:** Linux notifications have **Show it** and **Always allow**; the Windows balloon only supports "click to show".

**How:** modern toast notifications need an AppUserModelID (AUMID), and the shortcut that carries it.
- **Installer:** Inno Setup can set it on the Start menu shortcut with the `AppUserModelID:` parameter under `[Icons]`.
- **Code:** toasts come from `Windows.UI.Notifications`, via C++/WinRT (MSVC) or the raw WinRT COM interfaces.
- **Buttons:** clicks arrive by protocol activation (`focusguard:show?...`) or a COM activator.

The zip (portable) version has no shortcut, so it would keep the balloon.

## 3. Go to the last blocked window

A tray menu item ("Show Discord") plus an optional global shortcut, for when a block was wrong. The daemon already remembers the window for notifications; this exposes it.

## 4. Per-app grace period

**Why:** one global grace period is a compromise: pushy apps like Discord want a short one, slow but wanted apps (a browser opening a link) want a long one.

**How:** a syntax such as `grace-for = discord:300, firefox:2500` in the settings file, a column in the Lists page, and `decide()` using the window's own grace when present. Activity's timing hints already show which value would have helped.

## 5. Global shortcut to pause and resume

- **Windows:** `RegisterHotKey`.
- **X11:** `XGrabKey` on the root window.
- **Wayland:** the `GlobalShortcuts` portal (xdg-desktop-portal), where the desktop supports it.

Configurable in Settings, off by default.

## 6. "One action, one switch" (optional rule)

A deliberate action that already moved focus to one window doesn't also excuse a different window taking focus moments later. It fixes cases like "I clicked back into my terminal and Discord appeared within the grace period" without shortening the grace period.

The downside is clicking a link in an unfocused window: that click focuses the window and so is used up, and the browser opening right after would be blocked. So: an option, off by default.

## 7. Code signing for Windows

The SignPath Foundation signs open-source projects for free; signing runs as a CI step after the build. It removes the SmartScreen warning and makes antivirus false positives less likely.

## 8. More packages

- **AUR (Arch):** a `PKGBUILD` building from the release tag; publish to the AUR.
- **winget:** a manifest pointing at the installer and its SHA-256; `wingetcreate` can submit updates from CI.
- **.rpm:** CPack can build it from the same install rules (`cpack -G RPM`); needs testing on Fedora.
- **AppImage:** possible, but "start at login" must use the `$APPIMAGE` path rather than the temporary mount, and the Qt bundle adds about 30–40 MB.
- Not Flatpak or Snap: their sandboxes hide other apps' processes, which breaks allow and block rules by name.

## 9. Translations

The settings app's text already goes through `tr()`. It needs `.ts` files (Qt Linguist), loading the translation for the system language, and translators. The daemon's log lines can stay English; its notification texts and tray menu would need their own small catalogue.

## 10. End-to-end tests in CI

The unit tests cover the core, but the X11 daemon's behaviour (blocking, notification buttons, timed pause) was only checked by hand with a fake window manager and notification server on Xvfb. Moving that harness into `tests/` and running it in the Linux CI job would catch regressions in the parts unit tests can't reach.
