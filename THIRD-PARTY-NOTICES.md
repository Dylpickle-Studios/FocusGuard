# Third-party notices

FocusGuard itself is licensed under the GNU Affero General Public License, version 3 or (at your option) any later version (see [LICENSE](LICENSE)). Some release packages include or use the following third-party software.

## Qt

The Windows release bundles Qt 6 libraries and plugins (`Qt6*.dll` and the plugin folders) for `focusguard-settings.exe`. On Linux, the settings app uses the Qt libraries installed on your system.

Qt is copyright The Qt Company Ltd. and other contributors, and is used here under the **GNU Lesser General Public License v3.0** (<https://www.gnu.org/licenses/lgpl-3.0.html>). The Qt libraries are linked dynamically, so you can replace them with your own build. The Qt source code is available from <https://download.qt.io/official_releases/qt/> and <https://code.qt.io/>.

## Microsoft Visual C++ runtime

The Windows release includes the Microsoft Visual C++ runtime DLLs (`msvcp140*.dll`, `vcruntime140*.dll`, `concrt140.dll`) needed by the Qt libraries, redistributed under the terms of the Microsoft Visual Studio license for redistributable code.

## System libraries

On Linux, FocusGuard links to libX11, libXi (MIT/X11 license) and libdbus (Academic Free License 2.1 or GPL-2.0-or-later) as installed on your system; nothing of them is bundled.
