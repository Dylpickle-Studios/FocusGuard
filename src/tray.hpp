// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard - what the Linux tray icons (tray_sni.hpp, tray_xembed.hpp) share:
// the actions they ask for and the state they show.
#pragma once

namespace fg {

enum class TrayAction {
    TogglePause, Pause15, Pause60, ToggleStrict, ToggleMinimize, ToggleNotify, ToggleDryRun,
    ShowStats, EditConfig, Reload, Quit
};

struct TrayState {
    bool paused = false, strict = false, minimize = false, notify = false, dry_run = false;
    long long resume_at = 0;  // a timed pause's end (ms since the epoch), 0 if none
    bool operator==(const TrayState& o) const {
        return paused == o.paused && strict == o.strict && minimize == o.minimize && notify == o.notify &&
               dry_run == o.dry_run && resume_at == o.resume_at;
    }
    bool operator!=(const TrayState& o) const { return !(*this == o); }
};

}  // namespace fg
