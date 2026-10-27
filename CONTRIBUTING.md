# Contributing to FocusGuard

Thanks for helping! Bug reports, ideas and pull requests are all welcome.

## Reporting a problem

A window that FocusGuard blocks when it shouldn't (or lets through when it should block) is the most common report. These details make it quick to fix:

- your operating system, and on Linux the desktop or window manager;
- the FocusGuard version (`focusguard --version`);
- the log of that moment: run `focusguard --dry-run --verbose` (or open the Activity page in the settings app with **Show allowed changes too** on) and reproduce it. Each line shows the app, the window title and the reason for the decision. Remove anything private from window titles first.

## Building and testing

See [Building from source](README.md#building-from-source). Before sending a pull request:

```sh
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

The CI builds and tests Windows and Linux on every pull request.

## Code

- C++17, and no new dependencies beyond those listed in the README.
- The two programs (`src/focusguard_win.cpp`, `src/focusguard_x11.cpp`) each build as one file; shared logic belongs in `src/common.hpp` or `src/control.hpp`, with a unit test in `tests/tests.cpp`.
- Follow the style of the surrounding code (4-space indent, `snake_case` in the core, Qt naming in `gui/`).
- Keep FocusGuard itself light: it runs all day. Anything heavy belongs in the settings app.
- If you change the shield icon in `src/icon.hpp`, regenerate the Windows icons with `tools/make_icons.cpp` (instructions at the top of that file).

## License

By contributing, you agree that your contributions are licensed under the [GNU Affero General Public License, version 3 or later](LICENSE), like the rest of FocusGuard.
