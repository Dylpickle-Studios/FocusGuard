// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard - platform-independent core: configuration, matching, logging,
// statistics and the "is this focus change something the user asked for?"
// decision logic.
//
// Header-only so that each platform backend builds as one translation unit.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <filesystem>

#if defined(__GNUC__) || defined(__clang__)
#define FG_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define FG_PRINTF(fmt_idx, arg_idx)
#endif

namespace fg {

// The build passes the CMake project version; this fallback covers direct compiles.
#ifndef FOCUSGUARD_VERSION
#define FOCUSGUARD_VERSION "1.0.1"
#endif
constexpr const char* kVersion = FOCUSGUARD_VERSION;

// A process counts as "launched by you" if it started no more than this long
// after one of your deliberate actions (click, Enter, ...).
constexpr uint32_t kLaunchLeadMs = 2000;

// Upper bound for millisecond settings. Times are compared as signed 32-bit
// differences, so anything near 2^31 would silently break every comparison.
constexpr uint32_t kMaxMs = 24u * 60 * 60 * 1000;

// ------------------------------------------------------------------ strings

inline std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && lower(a) == lower(b);
}

inline bool istarts_with(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)prefix[i])) return false;
    return true;
}

// Normalised app name: lowercase, trimmed, no ".exe" ("Firefox.EXE" == "firefox").
inline std::string app_key(const std::string& s) {
    std::string k = lower(trim(s));
    if (k.size() > 4 && k.compare(k.size() - 4, 4, ".exe") == 0) k.resize(k.size() - 4);
    return k;
}

// Shorten to at most `max` bytes without cutting a UTF-8 sequence in half.
inline std::string ellipsize(std::string s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max > 3 ? max - 3 : 0;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
    s.resize(cut);
    return s + "...";
}

// "14:30": the local time of day for a wall-clock moment (ms since the epoch).
inline std::string local_hhmm(long long epoch_ms) {
    const std::time_t t = (std::time_t)(epoch_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

// Is version `a` ("1.2.3", optionally with a leading "v") newer than `b`?
// A suffix such as "-rc.1" is ignored; unparsable parts count as 0.
inline bool version_newer(const std::string& a, const std::string& b) {
    auto parts = [](std::string v) {
        if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
        std::array<unsigned long, 3> n{};
        const char* p = v.c_str();
        for (size_t i = 0; i < n.size() && *p; ++i) {
            char* end = nullptr;
            n[i] = std::strtoul(p, &end, 10);
            if (end == p || *end != '.') break;
            p = end + 1;
        }
        return n;
    };
    return parts(a) > parts(b);
}

// Case-insensitive glob with '*' (any run) and '?' (one char). `pat` must
// already be lowercased; `str` is lowered on the fly.
inline bool glob_ci(const std::string& pat, const std::string& str) {
    size_t p = 0, s = 0, star = std::string::npos, star_s = 0;
    while (s < str.size()) {
        const char c = (char)std::tolower((unsigned char)str[s]);
        if (p < pat.size() && (pat[p] == '?' || pat[p] == c)) {
            ++p;
            ++s;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            star_s = s;
        } else if (star != std::string::npos) {
            p = star + 1;
            s = ++star_s;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

inline FILE* open_utf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(std::filesystem::u8path(path).c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// ------------------------------------------------------------------ matching

// A window-name rule used by the allow and block lists.
//   firefox              exact process name / WM_CLASS (".exe" optional)
//   *updater*            glob; matched against process, class, instance and title
//   title:*- Notepad     glob matched against the window title only
//   title:Save As        exact (case-insensitive) title
struct Pattern {
    std::string text;  // app_key for exact names, lowercased otherwise
    bool glob = false;
    bool title_only = false;
};

inline Pattern compile_pattern(std::string raw) {
    Pattern p;
    raw = trim(raw);
    if (istarts_with(raw, "title:")) {
        p.title_only = true;
        raw = trim(raw.substr(6));
    }
    p.glob = raw.find_first_of("*?") != std::string::npos;
    p.text = (p.glob || p.title_only) ? lower(raw) : app_key(raw);
    return p;
}

class Matcher {
public:
    void build(const std::vector<std::string>& raw) {
        pats_.clear();
        for (const std::string& r : raw) {
            Pattern p = compile_pattern(r);
            if (!p.text.empty()) pats_.push_back(std::move(p));
        }
    }

    bool empty() const { return pats_.empty(); }
    size_t size() const { return pats_.size(); }

    bool match(const std::string& proc, const std::string& inst, const std::string& cls,
               const std::string& title) const {
        const std::string proc_key = app_key(proc);  // ".exe" is optional for globs too
        for (const Pattern& p : pats_) {
            if (p.title_only) {
                if (p.glob ? glob_ci(p.text, title) : iequals(p.text, title)) return true;
                continue;
            }
            if (p.glob) {
                if (glob_ci(p.text, proc) || glob_ci(p.text, proc_key) || glob_ci(p.text, inst) ||
                    glob_ci(p.text, cls) || glob_ci(p.text, title))
                    return true;
            } else {
                if (p.text == proc_key || p.text == app_key(inst) || p.text == app_key(cls))
                    return true;
            }
        }
        return false;
    }

private:
    std::vector<Pattern> pats_;
};

// ------------------------------------------------------------------ config

struct Config {
    uint32_t grace_ms = 1500;          // focus may change this long after a deliberate action
    uint32_t launch_grace_ms = 15000;  // apps you just launched may take focus for this long
    uint32_t fight_limit = 10;         // stop reverting a window after N reverts...
    uint32_t fight_window_ms = 3000;   // ...within this period (fight_limit 0 = never give up)
    bool dry_run = false;              // log only, never revert
    bool count_motion = false;         // mouse movement counts as intent (focus-follows-mouse)
    bool tray = true;                  // show a tray icon
    bool background = false;           // Windows: detach from the console
    int verbosity = 1;                 // 0 = errors only, 1 = blocks, 2 = everything

    // Which lenient rules are enabled (strict mode turns all three off).
    bool allow_same_app = true;
    bool allow_launched = true;
    bool allow_related = true;

    bool minimize_on_block = false;    // also minimise a blocked window
    bool notify = false;               // show a desktop notification on block
    uint32_t notify_cooldown_ms = 5000;
    bool history = true;               // keep the activity history and all-time statistics on disk

    std::string log_path;
    std::string config_path;           // file that was actually loaded (empty if none)
    std::vector<std::string> allow;    // raw patterns: always let these take focus
    std::vector<std::string> block;    // raw patterns: never take focus on their own (exemption list)
};

// ------------------------------------------------------------------ logging

struct LogState {
    int level = 1;
    FILE* file = nullptr;
    std::string path;
};

inline LogState& log_state() {
    static LogState s;
    return s;
}

FG_PRINTF(2, 3) inline void logmsg(int level, const char* fmt, ...) {
    LogState& ls = log_state();
    if (level > ls.level) return;

    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char ts[16];
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    std::fprintf(stderr, "[%s] %s\n", ts, msg);
    if (ls.file) {
        std::fprintf(ls.file, "[%s] %s\n", ts, msg);
        std::fflush(ls.file);
    }
}

// Apply logging settings; reopens the log file only when the path changed, so
// it is safe to call again on a live config reload.
inline void apply_logging(const Config& c) {
    LogState& ls = log_state();
    ls.level = c.verbosity;
    if (c.log_path == ls.path) return;
    if (ls.file) {
        std::fclose(ls.file);
        ls.file = nullptr;
    }
    ls.path = c.log_path;
    if (!c.log_path.empty()) {
        ls.file = open_utf8(c.log_path, "a");
        if (!ls.file) logmsg(0, "warning: cannot open log file '%s'", c.log_path.c_str());
    }
}

// ------------------------------------------------------------------ stats

class Stats {
public:
    void record(std::string app, uint32_t n = 1) {
        if (app.empty()) app = "?";
        total_ += n;
        counts_[app] += n;
    }
    void clear() {
        counts_.clear();
        total_ = 0;
    }
    uint32_t total() const { return total_; }

    void dump(const char* when) const {
        const char* w = when ? when : "";
        if (total_ == 0) {
            logmsg(0, "statistics: no windows blocked%s", w);
            return;
        }
        const auto v = ranked();
        logmsg(0, "statistics: %u windows blocked%s - top offenders:", (unsigned)total_, w);
        size_t shown = 0;
        for (const auto& e : v) {
            if (shown++ >= 10) {
                logmsg(0, "  ... and %u more", (unsigned)(v.size() - 10));
                break;
            }
            logmsg(0, "  %5u  %s", (unsigned)e.second, e.first.c_str());
        }
    }

    // Single-line summary for a tray tooltip / balloon.
    std::string top_line() const {
        if (total_ == 0) return "No windows blocked yet.";
        const auto v = ranked();
        std::string s = std::to_string(total_) + " blocked. Top: ";
        for (size_t i = 0; i < v.size() && i < 3; ++i) {
            if (i) s += ", ";
            s += v[i].first + " (" + std::to_string(v[i].second) + ")";
        }
        return s;
    }

    // Most blocked first, ties alphabetical, so every listing agrees.
    std::vector<std::pair<std::string, uint32_t>> ranked() const {
        std::vector<std::pair<std::string, uint32_t>> v(counts_.begin(), counts_.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        return v;
    }

private:
    std::unordered_map<std::string, uint32_t> counts_;
    uint32_t total_ = 0;
};

// ------------------------------------------------------------------ intent

enum class Key : uint8_t { Other, Enter, Escape, Alt, Super };

// Is this key event a deliberate "take me somewhere else" action?
// Plain typing deliberately is NOT: that is exactly when a focus steal hurts.
inline bool key_is_intent(Key k, bool down, bool alt_or_super_held) {
    if (!down) return k == Key::Alt || k == Key::Super;  // Alt+Tab commits / Start opens on release
    switch (k) {
        case Key::Enter:
        case Key::Escape:
        case Key::Alt:
        case Key::Super:
            return true;
        default:
            return alt_or_super_held;  // Alt+Tab, Win+1, Super+Arrow, ...
    }
}

// Remembers when the user last did something deliberate. Times are 32-bit
// millisecond clocks; all comparisons are wrap-safe signed differences.
class IntentTracker {
public:
    void mark(uint32_t t) {
        ring_[head_] = t;
        head_ = (head_ + 1) % ring_.size();
        if (count_ < ring_.size()) ++count_;
        touch(t);
    }

    // Weak, high-frequency activity (mouse motion, touch drags): only refreshes
    // the "recent" timestamp so it can't flood the launch history.
    void touch(uint32_t t) {
        last_ = t;
        have_last_ = true;
    }

    // Was there a deliberate action in [now - grace_ms, now]? Input after `now`
    // can't have caused the focus change, and must not hide an earlier action
    // that did, so the history is searched rather than just the latest event.
    bool recent(uint32_t now, uint32_t grace_ms) const {
        if (have_last_) {
            const int32_t d = (int32_t)(now - last_);
            if (d >= 0 && d <= (int32_t)grace_ms) return true;
        }
        return any_between(now - grace_ms, now);
    }

    bool any_between(uint32_t from, uint32_t to) const {
        for (size_t i = 0; i < count_; ++i) {
            const uint32_t t = ring_[i];
            if ((int32_t)(t - from) >= 0 && (int32_t)(to - t) >= 0) return true;
        }
        return false;
    }

    // Call when your action moved focus to another window. Remembers that
    // action (the latest one up to `now`), so launched_by_user can tell which
    // launches you have since moved on from.
    void switched(uint32_t now) {
        uint32_t t = 0;
        if (latest(now, t) && (!have_switch_ || (int32_t)(t - switch_) > 0)) {
            switch_ = t;
            have_switch_ = true;
        }
    }

    // Did you switch windows with an action later than `t`?
    bool switched_after(uint32_t t) const { return have_switch_ && (int32_t)(switch_ - t) > 0; }

    // Milliseconds from your latest deliberate action up to `now`, or -1 if
    // none is remembered. Shown with each decision to help tune the grace period.
    long long since_last(uint32_t now) const {
        uint32_t t = 0;
        return latest(now, t) ? (long long)(uint32_t)(now - t) : -1;
    }

private:
    // The most recent deliberate action at or before `now`.
    bool latest(uint32_t now, uint32_t& out) const {
        bool found = false;
        for (size_t i = 0; i < count_; ++i) {
            const int32_t d = (int32_t)(now - ring_[i]);
            if (d >= 0 && (!found || d < (int32_t)(now - out))) {
                out = ring_[i];
                found = true;
            }
        }
        return found;
    }

    std::array<uint32_t, 128> ring_{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint32_t last_ = 0;
    bool have_last_ = false;
    uint32_t switch_ = 0;
    bool have_switch_ = false;
};

// A process you started a moment ago may bring up its windows by itself, but
// only until you go elsewhere: once you switch windows after launching it, a
// late window (Discord's updater handing over to its main window, say) would
// otherwise pull you back.
inline bool launched_by_user(const IntentTracker& it, bool have_age, uint32_t age_ms, uint32_t now,
                             const Config& c) {
    if (!have_age || c.launch_grace_ms == 0 || age_ms > c.launch_grace_ms) return false;
    const uint32_t created = now - age_ms;
    const uint32_t latest = created + 250;  // +250: clock granularity
    return it.any_between(created - kLaunchLeadMs, latest) && !it.switched_after(latest);
}

// ------------------------------------------------------------------ decision

struct Facts {
    bool user_intent = false;       // deliberate action within grace period
    bool prev_usable = true;        // previous window still visible and not minimised
    bool same_app = false;          // new window belongs to the same process (rule enabled)
    bool related = false;           // new window is a dialog/child of the previous one (enabled)
    bool launched_by_user = false;  // process the user just started (rule enabled)
    bool allowlisted = false;       // matches the allow list
    bool blocklisted = false;       // matches the block/exemption list
};

struct Decision {
    bool allow;
    const char* reason;
};

inline Decision decide(const Facts& f) {
    // Your own actions always win: a blocklisted app you click or Alt+Tab to is
    // one you want. The block list only takes away the automatic passes below
    // (allow list, same app, dialogs, freshly launched).
    if (f.user_intent) return {true, "user action"};
    if (!f.prev_usable) return {true, "previous window closed or minimised"};
    if (f.blocklisted) return {false, "blocklisted"};
    if (f.allowlisted) return {true, "allowlisted"};
    if (f.same_app) return {true, "same application"};
    if (f.related) return {true, "dialog of the previous window"};
    if (f.launched_by_user) return {true, "app you just launched"};
    return {false, "no user action"};
}

// Some programs fight back and re-grab focus in a loop. After `fight_limit`
// reverts inside `fight_window_ms` we let them win instead of flickering forever.
class FightTracker {
public:
    bool keep_fighting(uint64_t id, uint32_t now, const Config& c) {
        if (c.fight_limit == 0) return true;
        if (map_.size() > 512) map_.clear();
        Entry& e = map_[id];
        if (e.count == 0 || (uint32_t)(now - e.first) > c.fight_window_ms) {
            e.first = now;
            e.count = 0;
        }
        return ++e.count <= c.fight_limit;
    }

private:
    struct Entry {
        uint32_t first = 0;
        uint32_t count = 0;
    };
    std::unordered_map<uint64_t, Entry> map_;
};

// Rate-limits desktop notifications so a storm of blocks can't spam the user.
class NotifyGate {
public:
    bool allow(uint32_t now, const Config& c) {
        if (have_ && (int32_t)(now - last_) < (int32_t)c.notify_cooldown_ms) return false;
        last_ = now;
        have_ = true;
        return true;
    }

private:
    uint32_t last_ = 0;
    bool have_ = false;
};

// ------------------------------------------------------------------ parsing

inline bool parse_uint(const std::string& s, uint32_t& out) {
    if (s.empty() || !std::isdigit((unsigned char)s[0])) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno || *end || v > 0xFFFFFFFFull) return false;
    out = (uint32_t)v;
    return true;
}

inline bool parse_bool(const std::string& s, bool& out) {
    const std::string v = lower(trim(s));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return out = true, true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return out = false, true;
    return false;
}

inline void split_into(const std::string& val, std::vector<std::string>& out) {
    size_t start = 0;
    while (start <= val.size()) {
        size_t comma = val.find(',', start);
        if (comma == std::string::npos) comma = val.size();
        const std::string item = trim(val.substr(start, comma - start));
        if (!item.empty()) out.push_back(item);
        start = comma + 1;
    }
}

// One setting, as used by both the config file and long command-line options.
inline bool apply_setting(Config& c, std::string key, const std::string& val, std::string& err) {
    key = lower(trim(key));
    for (char& ch : key)
        if (ch == '_') ch = '-';

    auto num = [&](uint32_t& dst) {
        if (parse_uint(val, dst)) return true;
        err = "'" + key + "' needs a non-negative number, got '" + val + "'";
        return false;
    };
    auto ms = [&](uint32_t& dst) {
        uint32_t v = 0;
        if (!num(v)) return false;
        if (v > kMaxMs) {
            err = "'" + key + "' must be at most " + std::to_string(kMaxMs) + " ms (24 h), got '" + val + "'";
            return false;
        }
        dst = v;
        return true;
    };
    auto flag = [&](bool& dst) {
        if (parse_bool(val, dst)) return true;
        err = "'" + key + "' needs yes/no, got '" + val + "'";
        return false;
    };

    if (key == "grace") return ms(c.grace_ms);
    if (key == "launch-grace") return ms(c.launch_grace_ms);
    if (key == "fight-limit") return num(c.fight_limit);
    if (key == "notify-cooldown") return ms(c.notify_cooldown_ms);
    if (key == "dry-run") return flag(c.dry_run);
    if (key == "count-motion") return flag(c.count_motion);
    if (key == "tray") return flag(c.tray);
    if (key == "background") return flag(c.background);
    if (key == "minimize-on-block" || key == "minimise-on-block") return flag(c.minimize_on_block);
    if (key == "notify") return flag(c.notify);
    if (key == "allow-same-app") return flag(c.allow_same_app);
    if (key == "allow-launched") return flag(c.allow_launched);
    if (key == "allow-related") return flag(c.allow_related);
    if (key == "history") return flag(c.history);
    if (key == "log") return c.log_path = val, true;
    if (key == "strict") {  // "strict = no" turns the lenient rules back on
        bool b = true;
        if (!val.empty() && !flag(b)) return false;
        c.allow_same_app = c.allow_launched = c.allow_related = !b;
        return true;
    }
    if (key == "verbose" || key == "quiet") {
        bool b = true;
        if (!val.empty() && !flag(b)) return false;
        const int level = key == "verbose" ? 2 : 0;
        if (b) c.verbosity = level;
        else if (c.verbosity == level) c.verbosity = 1;
        return true;
    }
    if (key == "allow") return split_into(val, c.allow), true;
    if (key == "block" || key == "deny" || key == "exempt") return split_into(val, c.block), true;
    err = "unknown setting '" + key + "'";
    return false;
}

// Reads one whole line, however long (fgets alone would split long lists).
inline bool read_line(FILE* f, std::string& out) {
    out.clear();
    char buf[1024];
    while (std::fgets(buf, sizeof buf, f)) {
        out += buf;
        if (out.back() == '\n') return true;
    }
    return !out.empty();
}

// Returns 0 on success, -1 if the file can't be opened, 1 on a syntax error.
inline int load_config_file(const std::string& path, Config& c, std::string& err) {
    FILE* f = open_utf8(path, "r");
    if (!f) return -1;
    std::string line;
    int lineno = 0, rc = 0;
    while (read_line(f, line)) {
        if (++lineno == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // BOM
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        const std::string key = trim(line.substr(0, eq));
        const std::string val = eq == std::string::npos ? "yes" : trim(line.substr(eq + 1));
        std::string e;
        if (!apply_setting(c, key, val, e)) {
            err = path + ":" + std::to_string(lineno) + ": " + e;
            rc = 1;
            break;
        }
    }
    std::fclose(f);
    return rc;
}

inline void print_usage(const std::string& default_config) {
    std::printf(
        "FocusGuard %s - stops windows from stealing keyboard focus.\n"
        "\n"
        "Usage: focusguard [options]\n"
        "\n"
        "Timing\n"
        "  -g, --grace MS         focus may change within MS of a deliberate action\n"
        "                         (click, Enter, Esc, Alt/Win shortcuts)         [1500]\n"
        "  -L, --launch-grace MS  apps you just launched may take focus for MS    [15000]\n"
        "      --fight-limit N    stop fighting a window after N reverts in 3 s   [10]\n"
        "                         (0 = never give up)\n"
        "\n"
        "Lists (NAME = process name / X11 WM_CLASS; globs with * ? allowed;\n"
        "       'title:PATTERN' matches the window title; repeatable, comma-separated)\n"
        "  -a, --allow NAME       always let NAME take focus\n"
        "  -b, --block NAME       never let NAME take focus on its own, even right\n"
        "                         after you launch it (your clicks still work)\n"
        "\n"
        "Rules\n"
        "      --strict           disable the lenient rules below (only your actions\n"
        "                         and the allow list may change focus)\n"
        "      --no-same-app      don't auto-allow an app's own windows\n"
        "      --no-launched      don't auto-allow apps you just launched\n"
        "      --no-related       don't auto-allow dialogs of the current window\n"
        "\n"
        "On a block\n"
        "      --minimize         also minimise the window that tried to steal focus\n"
        "      --notify           show a desktop notification\n"
        "  -n, --dry-run          only log what would be blocked, revert nothing\n"
        "\n"
        "Logging & misc\n"
        "  -v, --verbose          also log allowed focus changes\n"
        "  -q, --quiet            only log errors\n"
        "      --count-motion     treat mouse movement as intent (focus-follows-mouse)\n"
        "      --log FILE         also append log lines to FILE\n"
        "  -c, --config FILE      config file  [%s]\n"
        "      --print-config     show the effective settings and exit\n"
        "      --no-tray          don't show a tray icon\n"
        "      --background       (Windows) detach from the console\n"
        "  -h, --help             show this help\n"
        "  -V, --version          show the version\n"
        "\n"
        "Runtime control\n"
        "  Linux:   pkill -USR1 focusguard   pause / resume\n"
        "           pkill -USR2 focusguard   print statistics\n"
        "           pkill -HUP  focusguard   reload settings\n"
        "  Or use the tray icon's menu (Windows; Linux with a StatusNotifierItem tray).\n",
        kVersion, default_config.empty() ? "none" : default_config.c_str());
}

inline void print_effective_config(const Config& c) {
    auto yn = [](bool b) { return b ? "yes" : "no"; };
    std::printf("FocusGuard %s - effective settings\n", kVersion);
    std::printf("  config file       %s\n", c.config_path.empty() ? "(none)" : c.config_path.c_str());
    std::printf("  grace             %u ms\n", (unsigned)c.grace_ms);
    std::printf("  launch-grace      %u ms\n", (unsigned)c.launch_grace_ms);
    std::printf("  fight-limit       %u\n", (unsigned)c.fight_limit);
    std::printf("  allow same-app    %s\n", yn(c.allow_same_app));
    std::printf("  allow launched    %s\n", yn(c.allow_launched));
    std::printf("  allow related     %s\n", yn(c.allow_related));
    std::printf("  minimize on block %s\n", yn(c.minimize_on_block));
    std::printf("  notify            %s (cooldown %u ms)\n", yn(c.notify), (unsigned)c.notify_cooldown_ms);
    std::printf("  dry-run           %s\n", yn(c.dry_run));
    std::printf("  count-motion      %s\n", yn(c.count_motion));
    std::printf("  tray icon         %s\n", yn(c.tray));
    std::printf("  history           %s\n", yn(c.history));
    std::printf("  verbosity         %d\n", c.verbosity);
    std::printf("  log file          %s\n", c.log_path.empty() ? "(none)" : c.log_path.c_str());
    std::printf("  allow list (%u)%s\n", (unsigned)c.allow.size(), c.allow.empty() ? "   (empty)" : "");
    for (const std::string& a : c.allow) std::printf("      %s\n", a.c_str());
    std::printf("  block list (%u)%s\n", (unsigned)c.block.size(), c.block.empty() ? "   (empty)" : "");
    for (const std::string& b : c.block) std::printf("      %s\n", b.c_str());
}

enum class ParseResult { Run, ExitOk, ExitError };

// Config file first, then command-line options override it. Fills `c` fresh;
// call it again (with the same args) to re-apply settings on a live reload.
inline ParseResult parse_command_line(const std::vector<std::string>& args, Config& c,
                                      const std::string& default_config) {
    bool want_print = false;
    std::string cfg_path = default_config;
    bool explicit_cfg = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if ((args[i] == "-c" || args[i] == "--config") && i + 1 < args.size()) {
            cfg_path = args[++i];
            explicit_cfg = true;
        } else if (args[i].rfind("--config=", 0) == 0) {
            cfg_path = args[i].substr(9);
            explicit_cfg = true;
        }
    }
    if (!cfg_path.empty()) {
        std::string err;
        const int r = load_config_file(cfg_path, c, err);
        if (r == 1) {
            std::fprintf(stderr, "focusguard: %s\n", err.c_str());
            return ParseResult::ExitError;
        }
        if (r == -1 && explicit_cfg) {
            std::fprintf(stderr, "focusguard: cannot read config file '%s'\n", cfg_path.c_str());
            return ParseResult::ExitError;
        }
        if (r == 0) c.config_path = cfg_path;
    }

    for (size_t i = 1; i < args.size(); ++i) {
        std::string a = args[i], inline_val;
        bool has_inline = false;
        if (a.rfind("--", 0) == 0) {
            const size_t eq = a.find('=');
            if (eq != std::string::npos) {
                inline_val = a.substr(eq + 1);
                a.resize(eq);
                has_inline = true;
            }
        }
        const bool takes_value = a == "-c" || a == "--config" || a == "-g" || a == "--grace" ||
                                 a == "-L" || a == "--launch-grace" || a == "-a" || a == "--allow" ||
                                 a == "-b" || a == "--block" || a == "--log" || a == "--fight-limit" ||
                                 a == "--notify-cooldown";
        if (has_inline && !takes_value) {
            // --flag=VALUE means the same as the config file's key (--strict=no, --tray=no).
            std::string key = a.substr(2), err;
            if (key == "minimize" || key == "minimise") key = "minimize-on-block";
            if (key == "help" || key == "version" || key == "print-config" || key.rfind("no-", 0) == 0) {
                std::fprintf(stderr, "focusguard: %s doesn't take a value\n", a.c_str());
                return ParseResult::ExitError;
            }
            if (!apply_setting(c, key, inline_val, err)) {
                std::fprintf(stderr, "focusguard: %s (try --help)\n", err.c_str());
                return ParseResult::ExitError;
            }
            continue;
        }
        auto value = [&](std::string& out) {
            if (has_inline) return out = inline_val, true;
            if (i + 1 < args.size()) return out = args[++i], true;
            std::fprintf(stderr, "focusguard: %s needs a value\n", a.c_str());
            return false;
        };

        if (a == "-h" || a == "--help") {
            print_usage(default_config);
            return ParseResult::ExitOk;
        } else if (a == "-V" || a == "--version") {
            std::printf("focusguard %s\n", kVersion);
            return ParseResult::ExitOk;
        } else if (a == "--print-config") {
            want_print = true;
        } else if (a == "-n" || a == "--dry-run") {
            c.dry_run = true;
        } else if (a == "-v" || a == "--verbose") {
            c.verbosity = 2;
        } else if (a == "-q" || a == "--quiet") {
            c.verbosity = 0;
        } else if (a == "--count-motion") {
            c.count_motion = true;
        } else if (a == "--strict") {
            c.allow_same_app = c.allow_launched = c.allow_related = false;
        } else if (a == "--no-same-app") {
            c.allow_same_app = false;
        } else if (a == "--no-launched") {
            c.allow_launched = false;
        } else if (a == "--no-related") {
            c.allow_related = false;
        } else if (a == "--minimize" || a == "--minimise") {
            c.minimize_on_block = true;
        } else if (a == "--notify") {
            c.notify = true;
        } else if (a == "--no-tray") {
            c.tray = false;
        } else if (a == "--background") {
            c.background = true;
        } else if (a == "-c" || a == "--config") {
            std::string ignored;
            if (!value(ignored)) return ParseResult::ExitError;  // already handled above
        } else if (takes_value) {
            std::string v, err;
            if (!value(v)) return ParseResult::ExitError;
            const std::string key = a == "-g"  ? "grace"
                                    : a == "-L" ? "launch-grace"
                                    : a == "-a" ? "allow"
                                    : a == "-b" ? "block"
                                                : a.substr(2);
            if (!apply_setting(c, key, v, err)) {
                std::fprintf(stderr, "focusguard: %s\n", err.c_str());
                return ParseResult::ExitError;
            }
        } else {
            std::fprintf(stderr, "focusguard: unknown option '%s' (try --help)\n", a.c_str());
            return ParseResult::ExitError;
        }
    }

    if (want_print) {
        print_effective_config(c);
        return ParseResult::ExitOk;
    }
    return ParseResult::Run;
}

// ------------------------------------------------------------------ overrides

// Settings the tray menu flips at runtime.
enum class Toggle { Strict, Minimize, Notify, DryRun };

inline bool toggle_value(const Config& c, Toggle t) {
    switch (t) {
        case Toggle::Strict: return !(c.allow_same_app && c.allow_launched && c.allow_related);
        case Toggle::Minimize: return c.minimize_on_block;
        case Toggle::Notify: return c.notify;
        case Toggle::DryRun: return c.dry_run;
    }
    return false;
}

inline void set_toggle(Config& c, Toggle t, bool v) {
    switch (t) {
        case Toggle::Strict: c.allow_same_app = c.allow_launched = c.allow_related = !v; break;
        case Toggle::Minimize: c.minimize_on_block = v; break;
        case Toggle::Notify: c.notify = v; break;
        case Toggle::DryRun: c.dry_run = v; break;
    }
}

// The config-file key each toggle corresponds to.
inline const char* toggle_key(Toggle t) {
    switch (t) {
        case Toggle::Strict: return "strict";
        case Toggle::Minimize: return "minimize-on-block";
        case Toggle::Notify: return "notify";
        case Toggle::DryRun: return "dry-run";
    }
    return "";
}

inline bool toggle_from_key(const std::string& key, Toggle& t) {
    for (Toggle c : {Toggle::Strict, Toggle::Minimize, Toggle::Notify, Toggle::DryRun})
        if (key == toggle_key(c)) return t = c, true;
    return false;
}

// Settings flipped at runtime rather than in the settings file. Each one
// survives a reload for as long as the file's own value for that setting
// stays what it was when it was flipped: an unrelated edit doesn't undo it,
// but changing that setting in the file still wins.
class Overrides {
public:
    // Flips `t` in `live`. `file` is the configuration the file (and command
    // line) gives, without overrides.
    void flip(Toggle t, const Config& file, Config& live) {
        const bool value = !toggle_value(live, t);
        set_toggle(live, t, value);
        drop(t);
        const bool base = toggle_value(file, t);
        if (value != base) items_.push_back({t, base, value});
    }

    // After a reload: `live` holds the freshly read `fresh`; puts the
    // overrides that still apply back on top of it.
    void reapply(const Config& fresh, Config& live) {
        for (size_t i = 0; i < items_.size();) {
            if (toggle_value(fresh, items_[i].t) == items_[i].base) {
                set_toggle(live, items_[i].t, items_[i].value);
                ++i;
            } else {
                items_.erase(items_.begin() + (long)i);
            }
        }
    }

    // (key, value) for each active override.
    std::vector<std::pair<std::string, bool>> list() const {
        std::vector<std::pair<std::string, bool>> out;
        for (const Item& it : items_) out.emplace_back(toggle_key(it.t), it.value);
        return out;
    }

private:
    void drop(Toggle t) {
        items_.erase(std::remove_if(items_.begin(), items_.end(), [t](const Item& it) { return it.t == t; }),
                     items_.end());
    }

    struct Item {
        Toggle t;
        bool base, value;
    };
    std::vector<Item> items_;
};

// ------------------------------------------------------------------ writing

// Canonical spelling of a config key ("minimise_on_block" -> "minimize-on-block").
inline std::string canonical_key(std::string key) {
    key = lower(trim(key));
    for (char& ch : key)
        if (ch == '_') ch = '-';
    if (key == "minimise-on-block") return "minimize-on-block";
    if (key == "deny" || key == "exempt") return "block";
    return key;
}

// Returns `original` (a config file's text) rewritten to hold `c`: existing
// keys are updated where they stand, so comments, blank lines and unknown
// lines survive; keys the file lacks are appended only if they differ from
// the defaults. "strict" lines are replaced by the three rules they stand for.
inline std::string write_config(const std::string& original, const Config& c) {
    const Config def;
    auto yn = [](bool b) { return std::string(b ? "yes" : "no"); };
    struct Scalar {
        const char* key;
        std::string value, def;
    };
    const std::vector<Scalar> scalars = {
        {"grace", std::to_string(c.grace_ms), std::to_string(def.grace_ms)},
        {"launch-grace", std::to_string(c.launch_grace_ms), std::to_string(def.launch_grace_ms)},
        {"fight-limit", std::to_string(c.fight_limit), std::to_string(def.fight_limit)},
        {"allow-same-app", yn(c.allow_same_app), yn(def.allow_same_app)},
        {"allow-related", yn(c.allow_related), yn(def.allow_related)},
        {"allow-launched", yn(c.allow_launched), yn(def.allow_launched)},
        {"minimize-on-block", yn(c.minimize_on_block), yn(def.minimize_on_block)},
        {"notify", yn(c.notify), yn(def.notify)},
        {"notify-cooldown", std::to_string(c.notify_cooldown_ms), std::to_string(def.notify_cooldown_ms)},
        {"dry-run", yn(c.dry_run), yn(def.dry_run)},
        {"count-motion", yn(c.count_motion), yn(def.count_motion)},
        {"tray", yn(c.tray), yn(def.tray)},
        {"history", yn(c.history), yn(def.history)},
        {"background", yn(c.background), yn(def.background)},
        {"log", c.log_path, def.log_path},
    };
    const std::string verbosity = c.verbosity == 2 ? "verbose = yes" : c.verbosity == 0 ? "quiet = yes" : "";

    auto list_lines = [](const char* key, const std::vector<std::string>& items) {
        std::vector<std::string> out;
        for (const std::string& it : items) {
            std::vector<std::string> parts;  // an entry can't hold a comma: it would be split on load
            split_into(it, parts);
            for (const std::string& p : parts) out.push_back(std::string(key) + " = " + p);
        }
        return out;
    };
    auto scalar_line = [](const Scalar& sc) { return std::string(sc.key) + " = " + sc.value; };

    std::vector<std::string> out;
    std::vector<std::string> done;  // keys already written
    auto is_done = [&](const std::string& k) { return std::find(done.begin(), done.end(), k) != done.end(); };

    size_t pos = 0;
    bool first = true;
    while (pos < original.size()) {
        size_t nl = original.find('\n', pos);
        if (nl == std::string::npos) nl = original.size();
        std::string line = original.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = line;
        if (first && t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);  // BOM
        first = false;
        t = trim(t);
        if (t.empty() || t[0] == '#' || t[0] == ';') {
            out.push_back(line);
            continue;
        }
        const std::string key = canonical_key(t.substr(0, t.find('=')));
        auto sc = std::find_if(scalars.begin(), scalars.end(), [&](const Scalar& x) { return key == x.key; });
        if (sc != scalars.end()) {
            if (!is_done(key) && !(key == "log" && sc->value.empty())) out.push_back(scalar_line(*sc));
            done.push_back(key);
        } else if (key == "allow" || key == "block") {
            if (!is_done(key)) {
                const auto lines = list_lines(key.c_str(), key == "allow" ? c.allow : c.block);
                out.insert(out.end(), lines.begin(), lines.end());
            }
            done.push_back(key);
        } else if (key == "verbose" || key == "quiet") {
            if (!is_done("verbosity") && !verbosity.empty()) out.push_back(verbosity);
            done.push_back("verbosity");
        } else if (key == "strict") {
            continue;  // expressed by the three allow-* rules
        } else {
            out.push_back(line);  // unknown: keep it, the daemon will report it
        }
    }
    while (!out.empty() && trim(out.back()).empty()) out.pop_back();

    std::vector<std::string> added;
    for (const Scalar& sc : scalars)
        if (!is_done(sc.key) && sc.value != sc.def) added.push_back(scalar_line(sc));
    if (!is_done("verbosity") && !verbosity.empty()) added.push_back(verbosity);
    for (const char* key : {"allow", "block"}) {
        if (is_done(key)) continue;
        const auto lines = list_lines(key, std::string(key) == "allow" ? c.allow : c.block);
        added.insert(added.end(), lines.begin(), lines.end());
    }
    if (!added.empty()) {
        if (!out.empty()) out.push_back("");
        out.push_back("# Added by FocusGuard Settings");
        out.insert(out.end(), added.begin(), added.end());
    }

    std::string text;
    for (const std::string& l : out) text += l + "\n";
    return text;
}

// Adds `entry` to the allow (or block) list in the settings file at `path`,
// creating the file if needed and taking it off the other list. The file is
// rewritten in place (see write_config) and replaced atomically. Returns false
// with a message in `err` if the file can't be read, parsed or written.
inline bool add_to_config_file(const std::string& path, bool allow, const std::string& entry, std::string& err) {
    std::string text;
    if (FILE* f = open_utf8(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    Config c;
    if (load_config_file(path, c, err) == 1) return false;
    std::vector<std::string>& list = allow ? c.allow : c.block;
    std::vector<std::string>& other = allow ? c.block : c.allow;
    auto same = [&](const std::string& x) { return iequals(trim(x), entry); };
    other.erase(std::remove_if(other.begin(), other.end(), same), other.end());
    if (std::none_of(list.begin(), list.end(), same)) list.push_back(entry);
    const std::string out = write_config(text, c);
    if (out == text) return true;

    namespace fs = std::filesystem;
    const fs::path file = fs::u8path(path);
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    const std::string tmp = path + ".tmp";
    FILE* f = open_utf8(tmp, "wb");
    if (!f) {
        err = "cannot write " + tmp;
        return false;
    }
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    if (std::fclose(f) != 0 || !ok) {
        err = "cannot write " + tmp;
        fs::remove(fs::u8path(tmp), ec);
        return false;
    }
    fs::rename(fs::u8path(tmp), file, ec);
    if (ec) {
        err = "cannot replace " + path + ": " + ec.message();
        fs::remove(fs::u8path(tmp), ec);
        return false;
    }
    return true;
}

// Did the command line set anything besides the config file? Such options
// override the file, which the settings app points out.
inline bool has_cli_overrides(const std::vector<std::string>& args) {
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-c" || a == "--config") {
            ++i;
            continue;
        }
        if (a.rfind("--config=", 0) == 0 || a == "--background") continue;
        return true;
    }
    return false;
}

}  // namespace fg
