# Changelog

All notable changes to FocusGuard. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [1.0.1] - 2026-10-27

### Added
- **Windows installer** (`focusguard-<version>-windows-x64-setup.exe`): installs for your user account without an administrator prompt, adds a Start menu entry, can start FocusGuard at login, upgrades in place and uninstalls cleanly. The zip remains as the portable option.
- **.deb package** for Debian, Ubuntu, Mint and other Debian-based distributions (Ubuntu 22.04 / Debian 12 or newer), with a menu entry and the Qt libraries pulled in automatically.
- **Pause for a while**: pause for 15 minutes or an hour from the tray menu or the settings app, after which FocusGuard resumes by itself. The settings app and the tray show when.
- **Notification buttons** on Linux: *Show it* brings the blocked window to the front, *Always allow* adds its app to the allow list. On Windows, clicking the notification shows the window.
- **Timing in Activity**: each decision shows how long after your last click, Alt+Tab, Enter or similar it came. Close calls are highlighted, with a hint on whether a shorter or longer grace period would have changed the outcome.
- **Welcome card**: the first time the settings app opens, it offers to start FocusGuard now and at every login.
- **Optional update check**: with *Check for new versions* turned on, the settings app asks GitHub for the latest release once a day and shows when a newer one is out. Off by default.
- The control channel accepts `pause MINUTES`.
- `cmake --install` also installs the README, changelog and example settings file under `share/doc/focusguard`.

### Fixed
- An app you had just launched could still take focus after you had switched to another window while it started, for example a slow app's main window appearing after its splash screen. Switching windows now ends an app's startup pass.

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

[1.0.1]: https://github.com/DylanBeMe/FocusGuard/releases/tag/v1.0.1
[1.0.0]: https://github.com/DylanBeMe/FocusGuard/releases/tag/v1.0.0
