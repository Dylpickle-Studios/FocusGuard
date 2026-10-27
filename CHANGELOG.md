# Changelog

All notable changes to FocusGuard. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-10-27

The first public release.

### Added
- **Focus protection** for Windows (7 and later) and Linux (X11 with an EWMH window manager): when a window takes focus without you asking for it, focus goes straight back to your window and the newcomer's taskbar entry flashes instead.
- **Intent detection:** clicks, Alt+Tab, Win/Super shortcuts, Enter and Esc count as asking for a focus change; plain typing doesn't. Apps you just launched may come to the front while they start.
- **Allow and block lists** by process name, Linux window class, wildcard pattern or window title.
- **Strict mode** with per-rule switches, **dry run** for tuning, **minimise** for persistent offenders, and rate-limited **desktop notifications**.
- **Tray icon** that shows the state at a glance (protecting, paused, dry run):
  - on Windows, in the notification area;
  - on Linux, for StatusNotifierItem trays (KDE, Xfce, Cinnamon, LXQt, GNOME with AppIndicator, ...) with the full menu, and for XEmbed trays (i3bar, polybar, stalonetray, ...) without a menu.
- **FocusGuard Settings**, a Qt 6 app for Windows and Linux:
  - status with pause/resume and start/stop;
  - live activity and all-time statistics;
  - every setting explained;
  - allow/block list editing with a window picker;
  - start at login.
- **History:** blocked windows and per-app statistics survive restarts (`history = yes`, on by default; stored in `~/.local/state/focusguard` or `%LOCALAPPDATA%\FocusGuard`).
- **Live reload** of the settings file; tray toggles stay in effect until the file itself changes that setting.
- Every settings-file key also works as `--key=value` on the command line.
- A local control channel, private to your user account, that the settings app uses: a Unix socket on Linux and a named pipe on Windows.

[1.0.0]: https://github.com/DylanBeMe/FocusGuard/releases/tag/v1.0.0
