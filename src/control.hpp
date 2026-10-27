// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard - the control channel between the daemon and the settings app.
//
// The daemon listens on a per-user local endpoint: a Unix socket on Linux, a
// named pipe on Windows. A client sends one command per line and gets one
// JSON object per line back:
//
//   status     -> {"type":"status", "paused":..., "blocked":..., "top":[...], ...}
//   events     -> {"type":"events", "items":[event, ...]}   recent decisions
//   windows    -> {"type":"windows", "items":[window, ...]} open windows, for the picker
//   pause | resume | reload | quit | subscribe -> {"type":"ok"}
//   clear-history | reset-stats                 -> {"type":"ok"}
//
// After "subscribe" the daemon also pushes {"type":"event", ...} for every
// focus decision and {"type":"status", ...} whenever its state changes.
#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>

#include "common.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fg {

// ------------------------------------------------------------------ endpoint

#ifdef _WIN32
// Pipe name without the \\.\pipe\ prefix (QLocalSocket adds it). Per logon
// session; the daemon also restricts the pipe to the current user.
inline std::string control_endpoint(unsigned long session_id) {
    return "FocusGuard-" + std::to_string(session_id);
}
#else
// Socket path, in the per-user runtime directory when there is one.
inline std::string control_endpoint() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (dir && *dir) return std::string(dir) + "/focusguard.sock";
    return "/tmp/focusguard-" + std::to_string(getuid()) + ".sock";
}
#endif

// ------------------------------------------------------------------ JSON

// Appends `s` as a JSON string. Invalid UTF-8 (legacy X11 titles) becomes
// U+FFFD so a client's JSON parser never rejects the whole message.
inline void json_str(std::string& out, const std::string& s) {
    out += '"';
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
            ++i;
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
            ++i;
        } else if (c < 0x80) {
            out += (char)c;
            ++i;
        } else {
            // Length of a valid UTF-8 sequence starting here, or 0.
            size_t len = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c <= 0xDF ? 2 : 0;
            if (len && i + len <= n) {
                for (size_t k = 1; k < len; ++k)
                    if (((unsigned char)s[i + k] & 0xC0) != 0x80) len = 0;
            } else {
                len = 0;
            }
            if (len) {
                out.append(s, i, len);
                i += len;
            } else {
                out += "\xEF\xBF\xBD";
                ++i;
            }
        }
    }
    out += '"';
}

// Builds one JSON object: JsonObj().str("type", "ok").num("pid", 1).done()
class JsonObj {
public:
    JsonObj& str(const char* key, const std::string& v) {
        sep(key);
        json_str(s_, v);
        return *this;
    }
    JsonObj& num(const char* key, long long v) {
        sep(key);
        s_ += std::to_string(v);
        return *this;
    }
    JsonObj& boolean(const char* key, bool v) {
        sep(key);
        s_ += v ? "true" : "false";
        return *this;
    }
    JsonObj& raw(const char* key, const std::string& json) {
        sep(key);
        s_ += json;
        return *this;
    }
    std::string done() const { return s_ + "}"; }

private:
    void sep(const char* key) {
        if (s_.size() > 1) s_ += ',';
        json_str(s_, key);
        s_ += ':';
    }
    std::string s_ = "{";
};

inline std::string json_array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? "," : "") + items[i];
    return out + "]";
}

// ------------------------------------------------------------------ events

struct Event {
    long long time_ms = 0;  // wall clock, ms since the Unix epoch
    std::string kind;       // blocked, allowed, would-block, gave-up, failed
    std::string app, process, cls, title, reason;
};

inline long long wall_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline std::string event_json(const Event& e, const char* type = nullptr) {
    JsonObj o;
    if (type) o.str("type", type);
    return o.num("time", e.time_ms)
        .str("kind", e.kind)
        .str("app", e.app)
        .str("process", e.process)
        .str("class", e.cls)
        .str("title", e.title)
        .str("reason", e.reason)
        .done();
}

// Reads back one line written by event_json(): a flat object of string and
// number values. Not a general JSON parser; returns false on anything else.
inline bool parse_event_json(const std::string& line, Event& e) {
    size_t i = 0;
    auto ws = [&] {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) ++i;
    };
    auto str = [&](std::string& out) {
        if (i >= line.size() || line[i] != '"') return false;
        ++i;
        out.clear();
        while (i < line.size() && line[i] != '"') {
            char c = line[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= line.size()) return false;
            c = line[i++];
            if (c == 'n') out += '\n';
            else if (c == 't') out += '\t';
            else if (c == 'r') out += '\r';
            else if (c == 'b') out += '\b';
            else if (c == 'f') out += '\f';
            else if (c == 'u') {  // only \u00XX is ever written (control characters)
                if (i + 4 > line.size()) return false;
                out += (char)std::strtol(line.substr(i, 4).c_str(), nullptr, 16);
                i += 4;
            } else out += c;  // \" \\ \/
        }
        if (i >= line.size()) return false;
        ++i;  // closing quote
        return true;
    };
    ws();
    if (i >= line.size() || line[i++] != '{') return false;
    e = Event{};
    for (;;) {
        ws();
        if (i < line.size() && line[i] == '}') break;
        std::string key, sval;
        if (!str(key)) return false;
        ws();
        if (i >= line.size() || line[i++] != ':') return false;
        ws();
        if (i < line.size() && line[i] == '"') {
            if (!str(sval)) return false;
            if (key == "kind") e.kind = sval;
            else if (key == "app") e.app = sval;
            else if (key == "process") e.process = sval;
            else if (key == "class") e.cls = sval;
            else if (key == "title") e.title = sval;
            else if (key == "reason") e.reason = sval;
        } else {
            const size_t start = i;
            while (i < line.size() && line[i] != ',' && line[i] != '}') ++i;
            if (key == "time") e.time_ms = std::strtoll(line.substr(start, i - start).c_str(), nullptr, 10);
        }
        ws();
        if (i < line.size() && line[i] == ',') {
            ++i;
            continue;
        }
        if (i < line.size() && line[i] == '}') break;
        return false;
    }
    return !e.kind.empty() && e.time_ms > 0;
}

// The most recent focus decisions, for clients that connect later.
class EventLog {
public:
    void add(Event e) {
        items_.push_back(std::move(e));
        if (items_.size() > kMax) items_.pop_front();
    }
    void clear() { items_.clear(); }
    std::string json() const {
        std::vector<std::string> v;
        for (const Event& e : items_) v.push_back(event_json(e));
        return JsonObj().str("type", "events").raw("items", json_array(v)).done();
    }

private:
    static constexpr size_t kMax = 300;
    std::deque<Event> items_;
};

// ------------------------------------------------------------------ history

// Keeps the activity history and the all-time statistics on disk so they
// survive restarts. Only decisions worth reviewing are stored (blocks, dry-run
// blocks, give-ups, failures): routine allowed switches stay in memory, so
// Alt+Tab doesn't mean disk writes.
//   events.jsonl  one event per line, appended; compacted to the newest lines
//   stats.tsv     "since<TAB>ms", then "count<TAB>app" per app
class History {
public:
    static constexpr size_t kKeep = 300;     // events kept after compaction
    static constexpr size_t kCompact = 2000; // lines that trigger a compaction

    // Loads what is stored in `dir` (created if needed). Returns false if the
    // directory can't be used; the history is then simply not kept.
    bool open(const std::string& dir, EventLog& log, Stats& all_time, long long& since) {
        close();
        std::error_code ec;
        std::filesystem::create_directories(path(dir), ec);
        if (!std::filesystem::is_directory(path(dir), ec)) return false;
        dir_ = dir;
        std::vector<std::string> lines = read_lines(events_file());
        for (const std::string& l : lines) {
            Event e;
            if (parse_event_json(l, e)) log.add(std::move(e));
        }
        lines_ = lines.size();
        if (lines_ > kKeep * 2) compact(lines);

        all_time.clear();
        since = 0;
        for (const std::string& l : read_lines(stats_file())) {
            const size_t tab = l.find('\t');
            if (tab == std::string::npos) continue;
            const std::string a = l.substr(0, tab), b = l.substr(tab + 1);
            if (a == "since") since = std::strtoll(b.c_str(), nullptr, 10);
            else if (!b.empty() && std::isdigit((unsigned char)a[0])) all_time.record(b, (uint32_t)std::strtoul(a.c_str(), nullptr, 10));
        }
        if (!since) since = wall_ms();
        return true;
    }

    void close() { dir_.clear(); }
    bool is_open() const { return !dir_.empty(); }

    void append(const Event& e) {
        if (!is_open() || e.kind == "allowed") return;
        if (FILE* f = open_utf8(events_file(), "a")) {
            const std::string line = event_json(e) + "\n";
            std::fwrite(line.data(), 1, line.size(), f);
            std::fclose(f);
            if (++lines_ >= kCompact) compact(read_lines(events_file()));
        }
    }

    void save_stats(const Stats& all_time, long long since) {
        if (!is_open()) return;
        std::string text = "since\t" + std::to_string(since) + "\n";
        for (const auto& e : all_time.ranked()) text += std::to_string(e.second) + "\t" + e.first + "\n";
        write_atomically(stats_file(), text);
    }

    void clear_events() {
        if (!is_open()) return;
        write_atomically(events_file(), "");
        lines_ = 0;
    }

private:
    static std::filesystem::path path(const std::string& utf8) {
#ifdef _WIN32
        return std::filesystem::u8path(utf8);
#else
        return utf8;
#endif
    }
    std::string events_file() const { return dir_ + "/events.jsonl"; }
    std::string stats_file() const { return dir_ + "/stats.tsv"; }

    static std::vector<std::string> read_lines(const std::string& file) {
        std::vector<std::string> out;
        if (FILE* f = open_utf8(file, "r")) {
            std::string line;
            while (read_line(f, line)) {
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                if (!line.empty()) out.push_back(line);
            }
            std::fclose(f);
        }
        return out;
    }

    // Write to a temporary file, then rename it over the old one, so a crash
    // never leaves a half-written file behind.
    static bool write_atomically(const std::string& file, const std::string& text) {
        const std::string tmp = file + ".tmp";
        FILE* f = open_utf8(tmp, "w");
        if (!f) return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        if (std::fclose(f) != 0 || !ok) return false;
        std::error_code ec;
        std::filesystem::rename(path(tmp), path(file), ec);
        return !ec;
    }

    void compact(const std::vector<std::string>& lines) {
        const size_t from = lines.size() > kKeep ? lines.size() - kKeep : 0;
        std::string text;
        for (size_t i = from; i < lines.size(); ++i) text += lines[i] + "\n";
        if (write_atomically(events_file(), text)) lines_ = lines.size() - from;
    }

    std::string dir_;
    size_t lines_ = 0;
};

// ------------------------------------------------------------------ windows

struct WindowInfo {
    std::string app, process, cls, instance, title;
    bool active = false, allowed = false, blocked = false;
};

inline std::string windows_json(const std::vector<WindowInfo>& ws) {
    std::vector<std::string> v;
    for (const WindowInfo& w : ws)
        v.push_back(JsonObj()
                        .str("app", w.app)
                        .str("process", w.process)
                        .str("class", w.cls)
                        .str("instance", w.instance)
                        .str("title", w.title)
                        .boolean("active", w.active)
                        .boolean("allowed", w.allowed)
                        .boolean("blocked", w.blocked)
                        .done());
    return JsonObj().str("type", "windows").raw("items", json_array(v)).done();
}

// ------------------------------------------------------------------ status

// Everything the status message reports.
struct StatusInfo {
    const Config& config;
    bool paused;
    const Stats& session;   // since the daemon started
    const Stats& all_time;  // persisted (equals `session` without history)
    long long since;        // start of the all-time statistics, ms since the epoch
    long long pid;
    std::string config_path;  // the file the daemon reads (or would read if it existed)
    bool cli_overrides;
    std::vector<std::pair<std::string, bool>> overrides;  // runtime toggles (Overrides::list)
};

inline std::string top_json(const Stats& stats) {
    std::vector<std::string> top;
    for (const auto& e : stats.ranked()) {
        if (top.size() == 10) break;
        top.push_back(JsonObj().str("app", e.first).num("count", e.second).done());
    }
    return json_array(top);
}

inline std::string status_json(const StatusInfo& s) {
    const Config& c = s.config;
    std::vector<std::string> overrides;
    for (const auto& o : s.overrides) overrides.push_back(JsonObj().str("key", o.first).boolean("value", o.second).done());
#ifdef _WIN32
    const char* platform = "windows";
#else
    const char* platform = "linux";
#endif
    return JsonObj()
        .str("type", "status")
        .str("version", kVersion)
        .str("platform", platform)
        .num("pid", s.pid)
        .boolean("paused", s.paused)
        .boolean("dry_run", c.dry_run)
        .boolean("strict", !(c.allow_same_app && c.allow_launched && c.allow_related))
        .boolean("notify", c.notify)
        .boolean("minimize", c.minimize_on_block)
        .boolean("history", c.history)
        .str("config", s.config_path)
        .boolean("config_loaded", !c.config_path.empty())
        .boolean("cli_overrides", s.cli_overrides)
        .raw("overrides", json_array(overrides))
        .num("blocked", s.session.total())
        .raw("top", top_json(s.session))
        .num("blocked_total", s.all_time.total())
        .raw("top_total", top_json(s.all_time))
        .num("since", s.since)
        .done();
}

// ------------------------------------------------------------------ commands

// What a backend provides to answer commands.
struct ControlHooks {
    std::function<std::string()> status;   // status_json(...)
    std::function<std::string()> windows;  // windows_json(...)
    std::function<void(bool)> set_paused;
    std::function<void()> reload;
    std::function<void()> quit;
    std::function<void()> clear_history;
    std::function<void()> reset_stats;
};

// Answers one command line. Returns the reply (newline-terminated); sets
// `subscribe` when the client asks for pushed updates.
inline std::string handle_control(const std::string& line, const EventLog& log, const ControlHooks& h,
                                  bool& subscribe) {
    const std::string cmd = lower(trim(line));
    const std::string ok = JsonObj().str("type", "ok").str("command", cmd).done();
    std::string reply;
    if (cmd == "status") reply = h.status();
    else if (cmd == "events") reply = log.json();
    else if (cmd == "windows") reply = h.windows();
    else if (cmd == "pause" || cmd == "resume") h.set_paused(cmd == "pause"), reply = ok;
    else if (cmd == "reload") h.reload(), reply = ok;
    else if (cmd == "quit") h.quit(), reply = ok;
    else if (cmd == "clear-history") h.clear_history(), reply = ok;
    else if (cmd == "reset-stats") h.reset_stats(), reply = ok;
    else if (cmd == "subscribe") subscribe = true, reply = ok;
    else if (cmd.empty()) return {};
    else reply = JsonObj().str("type", "error").str("message", "unknown command '" + cmd + "'").done();
    return reply + "\n";
}

}  // namespace fg
