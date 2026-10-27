// SPDX-License-Identifier: AGPL-3.0-or-later
// Unit tests for the platform-independent core (src/common.hpp, src/control.hpp).
// No framework: each TEST registers itself, CHECK records failures, and the
// program exits non-zero if any failed. Run with ctest or directly.
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "common.hpp"
#include "control.hpp"

namespace {

struct Test {
    const char* name;
    std::function<void()> fn;
};
std::vector<Test>& tests() {
    static std::vector<Test> t;
    return t;
}
int g_failures = 0;
const char* g_current = "";

struct Register {
    Register(const char* name, std::function<void()> fn) { tests().push_back({name, std::move(fn)}); }
};

#define TEST(name)                                         \
    void test_##name();                                    \
    const Register register_##name(#name, test_##name);    \
    void test_##name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            ++g_failures;                                                                  \
            std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_current, #cond);   \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

// A config parsed from text, the way the daemon reads its file.
fg::Config parse_text(const std::string& text, int* rc = nullptr, std::string* err = nullptr) {
    const auto path = std::filesystem::temp_directory_path() / "focusguard-test.conf";
    FILE* f = std::fopen(path.string().c_str(), "wb");
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    fg::Config c;
    std::string e;
    const int r = fg::load_config_file(path.string(), c, e);
    if (rc) *rc = r;
    if (err) *err = e;
    std::filesystem::remove(path);
    return c;
}

fg::Config parse_args(std::vector<std::string> args, fg::ParseResult* result = nullptr) {
    args.insert(args.begin(), "focusguard");
    fg::Config c;
    const fg::ParseResult r = fg::parse_command_line(args, c, "");
    if (result) *result = r;
    return c;
}

}  // namespace

// ------------------------------------------------------------------ strings & matching

TEST(glob) {
    CHECK(fg::glob_ci("*updater*", "GoogleUpdater"));
    CHECK(fg::glob_ci("f?ref*", "Firefox"));
    CHECK(fg::glob_ci("*", ""));
    CHECK(!fg::glob_ci("*updater", "updater.exe"));
    CHECK(!fg::glob_ci("abc", "abcd"));
    CHECK(fg::glob_ci("a*b*c", "aXXbYYc"));
}

TEST(app_key_and_ellipsize) {
    CHECK_EQ(fg::app_key("  Firefox.EXE "), "firefox");
    CHECK_EQ(fg::app_key(".exe"), ".exe");  // nothing left to strip it from
    CHECK_EQ(fg::ellipsize("short", 10), "short");
    // Never cut a UTF-8 sequence in half: "é" is two bytes.
    const std::string s = fg::ellipsize("aaaaé" "bbbbbbbb", 8);
    CHECK_EQ(s, "aaaa...");
}

TEST(matcher) {
    fg::Matcher m;
    m.build({"KeePassXC.exe", "*updater", "title:Save As", "title:*- Notepad", ",", "  "});
    CHECK_EQ(m.size(), 5u);  // "  " is dropped
    CHECK(m.match("keepassxc.exe", "", "", ""));      // exact, .exe optional
    CHECK(m.match("", "", "KeePassXC", ""));           // WM_CLASS
    CHECK(m.match("GoogleUpdater.exe", "", "", ""));   // glob, .exe optional
    CHECK(m.match("", "", "", "save as"));             // exact title, any case
    CHECK(!m.match("", "", "", "Save As..."));
    CHECK(m.match("", "", "", "notes.txt - Notepad"));
    CHECK(!m.match("notepad", "", "", ""));            // title patterns ignore names
}

// ------------------------------------------------------------------ settings

TEST(parse_values) {
    uint32_t v = 0;
    CHECK(fg::parse_uint("1500", v) && v == 1500);
    CHECK(!fg::parse_uint("-1", v));
    CHECK(!fg::parse_uint("12ms", v));
    CHECK(!fg::parse_uint("99999999999", v));
    bool b = false;
    CHECK(fg::parse_bool(" Yes ", b) && b);
    CHECK(fg::parse_bool("off", b) && !b);
    CHECK(!fg::parse_bool("maybe", b));
}

TEST(apply_setting) {
    fg::Config c;
    std::string err;
    CHECK(fg::apply_setting(c, "Launch_Grace", "2000", err) && c.launch_grace_ms == 2000);
    CHECK(!fg::apply_setting(c, "grace", "90000000", err));  // over the 24 h cap
    CHECK(err.find("at most") != std::string::npos);
    CHECK(fg::apply_setting(c, "strict", "yes", err) && !c.allow_same_app && !c.allow_launched && !c.allow_related);
    CHECK(fg::apply_setting(c, "strict", "no", err) && c.allow_same_app && c.allow_launched && c.allow_related);
    CHECK(fg::apply_setting(c, "minimise_on_block", "yes", err) && c.minimize_on_block);
    CHECK(fg::apply_setting(c, "exempt", "a, b", err) && c.block.size() == 2);
    CHECK(fg::apply_setting(c, "verbose", "", err) && c.verbosity == 2);
    CHECK(fg::apply_setting(c, "verbose", "no", err) && c.verbosity == 1);
    CHECK(fg::apply_setting(c, "history", "no", err) && !c.history);
    CHECK(!fg::apply_setting(c, "colour", "blue", err));
    CHECK(err.find("unknown setting") != std::string::npos);
}

TEST(command_line) {
    fg::ParseResult r;
    fg::Config c = parse_args({"--strict=no", "--dry-run=no", "--tray=no", "-g", "700", "--block=x,y"}, &r);
    CHECK(r == fg::ParseResult::Run);
    CHECK(c.allow_launched && !c.dry_run && !c.tray && c.grace_ms == 700 && c.block.size() == 2);
    c = parse_args({"--strict"});
    CHECK(!c.allow_launched);
    c = parse_args({"--minimize=yes", "--allow-launched=no"});
    CHECK(c.minimize_on_block && !c.allow_launched);
    parse_args({"--no-tray=1"}, &r);
    CHECK(r == fg::ParseResult::ExitError);
    parse_args({"--grace"}, &r);  // missing value
    CHECK(r == fg::ParseResult::ExitError);
    parse_args({"--frobnicate"}, &r);
    CHECK(r == fg::ParseResult::ExitError);
}

TEST(cli_overrides) {
    CHECK(!fg::has_cli_overrides({"focusguard"}));
    CHECK(!fg::has_cli_overrides({"focusguard", "-c", "x.conf", "--background", "--config=y"}));
    CHECK(fg::has_cli_overrides({"focusguard", "--quiet"}));
}

TEST(config_file) {
    int rc = 0;
    std::string err;
    fg::Config c = parse_text("\xEF\xBB\xBF# comment\n; also a comment\n\ngrace = 900\r\nnotify\nallow = a, b\n", &rc);
    CHECK_EQ(rc, 0);
    CHECK(c.grace_ms == 900 && c.notify && c.allow.size() == 2);

    std::string longline = "block = ";
    for (int i = 0; i < 400; ++i) longline += "app" + std::to_string(i) + ", ";
    c = parse_text(longline + "\n", &rc);
    CHECK(rc == 0 && c.block.size() == 400);

    parse_text("grace = 1\nbogus = 2\n", &rc, &err);
    CHECK_EQ(rc, 1);
    CHECK(err.find(":2:") != std::string::npos);
}

TEST(write_config) {
    const std::string orig =
        "# mine\ngrace = 1500\n\n# trusted\nallow = a, b\nallow = c\nstrict = yes\n"
        "minimise_on_block = no\nexempt = *updater*\nverbose = yes\n; old style\n";
    fg::Config c = parse_text(orig);
    c.grace_ms = 900;
    c.allow = {"a", "c"};
    c.allow_launched = true;
    c.block.push_back("title:Save As");
    c.notify = true;
    c.verbosity = 1;
    const std::string out = fg::write_config(orig, c);
    CHECK(out.find("# mine\ngrace = 900\n\n# trusted\nallow = a\nallow = c\n") == 0);
    CHECK(out.find("strict") == std::string::npos);
    CHECK(out.find("verbose") == std::string::npos);
    CHECK(out.find("; old style") != std::string::npos);
    CHECK(out.find("block = *updater*\nblock = title:Save As\n") != std::string::npos);

    const fg::Config back = parse_text(out);
    CHECK(back.grace_ms == 900 && back.allow == c.allow && back.block == c.block);
    CHECK(back.allow_launched && !back.allow_same_app && !back.allow_related && back.notify);
    CHECK_EQ(fg::write_config(out, back), out);  // stable

    CHECK(fg::write_config("", fg::Config{}).empty());
    fg::Config d;
    d.block = {"x,y"};
    CHECK_EQ(fg::write_config("", d), "# Added by FocusGuard Settings\nblock = x\nblock = y\n");
}

// ------------------------------------------------------------------ decisions

TEST(intent) {
    fg::IntentTracker t;
    CHECK(!t.recent(1000, 1500));
    t.mark(1000);
    CHECK(t.recent(2400, 1500));
    CHECK(!t.recent(2600, 1500));
    CHECK(!t.recent(900, 1500));  // input after the focus change didn't cause it
    t.mark(1020);                 // a later keypress must not hide the earlier click
    CHECK(t.recent(1010, 1500));
    fg::IntentTracker m;
    m.touch(5000);
    CHECK(m.recent(5100, 1500));
    CHECK(!m.any_between(4000, 6000));  // motion never counts as a launch action
    fg::IntentTracker w;                // 32-bit clock wrap
    w.mark(0xFFFFFF00u);
    CHECK(w.recent(0x00000100u, 1500));
}

TEST(launched) {
    fg::Config c;
    fg::IntentTracker t;
    t.mark(10000);
    CHECK(fg::launched_by_user(t, true, 1000, 12000, c));   // started 1 s after the click
    CHECK(!fg::launched_by_user(t, true, 3000, 12000, c));  // started before it
    CHECK(!fg::launched_by_user(t, true, 3000, 20000, c));  // started 7 s after it
    CHECK(!fg::launched_by_user(t, true, 20000, 30000, c)); // older than the launch grace
    CHECK(!fg::launched_by_user(t, false, 0, 12000, c));
    c.launch_grace_ms = 0;
    CHECK(!fg::launched_by_user(t, true, 1000, 12000, c));
}

TEST(launched_until_you_move_on) {
    fg::Config c;
    // Enter in a launcher at 10 s starts Discord at 10.1 s; the launcher closing
    // hands focus back (a switch, but caused by that same Enter): still a launch.
    fg::IntentTracker t;
    t.mark(10000);
    t.switched(10150);
    CHECK(fg::launched_by_user(t, true, 3900, 14000, c));
    // You click back into the terminal at 12 s; Discord's main window shows up at
    // 16 s and no longer counts as just launched.
    t.mark(12000);
    t.switched(12050);
    CHECK(!fg::launched_by_user(t, true, 5900, 16000, c));
    // A click that didn't switch windows (typing in the terminal) changes nothing.
    fg::IntentTracker u;
    u.mark(10000);
    u.mark(12000);
    CHECK(fg::launched_by_user(u, true, 5900, 16000, c));
    // Apps launched after the switch get their pass as usual.
    t.mark(20000);
    CHECK(fg::launched_by_user(t, true, 500, 20600, c));
    // A switch is remembered by its action, so an earlier one can't undo a later one.
    t.switched(11000);
    CHECK(!fg::launched_by_user(t, true, 5900, 16000, c));
}

TEST(decide) {
    fg::Facts f;
    CHECK(!fg::decide(f).allow);
    f.user_intent = true, f.blocklisted = true;
    CHECK(fg::decide(f).allow);  // your own action beats the block list
    f = {};
    f.prev_usable = false;
    CHECK(fg::decide(f).allow);
    f = {};
    f.allowlisted = true, f.blocklisted = true;
    CHECK(!fg::decide(f).allow);  // block wins over allow
    f = {};
    f.same_app = true;
    CHECK(fg::decide(f).allow);
    f.blocklisted = true;
    CHECK(!fg::decide(f).allow);
}

TEST(fight_and_notify) {
    fg::Config c;
    c.fight_limit = 3;
    fg::FightTracker ft;
    CHECK(ft.keep_fighting(1, 0, c) && ft.keep_fighting(1, 10, c) && ft.keep_fighting(1, 20, c));
    CHECK(!ft.keep_fighting(1, 30, c));
    CHECK(ft.keep_fighting(1, 5000, c));  // a new window of time
    CHECK(ft.keep_fighting(2, 30, c));    // other windows are separate
    c.fight_limit = 0;
    for (int i = 0; i < 100; ++i) CHECK(ft.keep_fighting(3, (uint32_t)i, c));

    fg::NotifyGate g;
    c.notify_cooldown_ms = 5000;
    CHECK(g.allow(0, c));
    CHECK(!g.allow(4000, c));
    CHECK(g.allow(6000, c));
}

TEST(stats) {
    fg::Stats s;
    s.record("b");
    s.record("a");
    s.record("c", 3);
    s.record("");
    CHECK_EQ(s.total(), 6u);
    const auto r = s.ranked();
    CHECK(r.size() == 4 && r[0].first == "c" && r[1].first == "?" && r[2].first == "a");  // ties alphabetical
    CHECK(s.top_line().find("6 blocked") == 0);
    s.clear();
    CHECK(s.total() == 0 && s.ranked().empty());
}

TEST(overrides) {
    fg::Config file;
    fg::Config live = file;
    fg::Overrides o;
    o.flip(fg::Toggle::DryRun, file, live);
    CHECK(live.dry_run);
    CHECK(o.list().size() == 1 && o.list()[0].first == "dry-run" && o.list()[0].second);

    // An unrelated edit keeps the override.
    fg::Config fresh = file;
    fresh.grace_ms = 800;
    live = fresh;
    o.reapply(fresh, live);
    CHECK(live.dry_run && live.grace_ms == 800);

    // Changing that setting in the file wins and drops the override.
    fresh.dry_run = true;
    live = fresh;
    o.reapply(fresh, live);
    CHECK(o.list().empty());
    fresh.dry_run = false;
    live = fresh;
    o.reapply(fresh, live);
    CHECK(!live.dry_run);

    // Flipping back to the file's value is no override at all.
    o.flip(fg::Toggle::Notify, file, live);
    o.flip(fg::Toggle::Notify, file, live);
    CHECK(o.list().empty() && !live.notify);

    // Strict covers the three rules.
    o.flip(fg::Toggle::Strict, file, live);
    CHECK(!live.allow_same_app && !live.allow_launched && !live.allow_related);
    live = file;
    o.reapply(file, live);
    CHECK(fg::toggle_value(live, fg::Toggle::Strict));

    fg::Toggle t;
    CHECK(fg::toggle_from_key("minimize-on-block", t) && t == fg::Toggle::Minimize);
    CHECK(!fg::toggle_from_key("grace", t));
}

// ------------------------------------------------------------------ control channel

TEST(json) {
    std::string s;
    fg::json_str(s, "a\"b\\c\n\x01");
    CHECK_EQ(s, "\"a\\\"b\\\\c\\u000a\\u0001\"");
    s.clear();
    fg::json_str(s, "caf\xC3\xA9 \xF0\x9F\x9B\xA1");  // valid UTF-8 passes through
    CHECK_EQ(s, "\"caf\xC3\xA9 \xF0\x9F\x9B\xA1\"");
    s.clear();
    fg::json_str(s, "bad\xE9!\xC3");  // invalid bytes become U+FFFD
    CHECK_EQ(s, "\"bad\xEF\xBF\xBD!\xEF\xBF\xBD\"");
    CHECK_EQ(fg::JsonObj().str("a", "x").num("n", -5).boolean("b", true).raw("r", "[]").done(),
             "{\"a\":\"x\",\"n\":-5,\"b\":true,\"r\":[]}");
}

TEST(event_round_trip) {
    fg::Event e{1793100000123LL, "blocked", "Popup", "popup", "Popup", "Update \"ready\" \\ now\n\x02", "no user action"};
    fg::Event back;
    CHECK(fg::parse_event_json(fg::event_json(e), back));
    CHECK(back.time_ms == e.time_ms && back.kind == e.kind && back.title == e.title && back.reason == e.reason);
    CHECK(fg::parse_event_json(fg::event_json(e, "event"), back));  // extra keys are fine
    CHECK(!fg::parse_event_json("", back));
    CHECK(!fg::parse_event_json("{\"kind\":\"blocked\"", back));
    CHECK(!fg::parse_event_json("{\"time\":5}", back));  // no kind
    CHECK(!fg::parse_event_json("not json", back));
    CHECK(back.after_ms == -1 && fg::event_json(e).find("after") == std::string::npos);  // unknown: left out
    e.after_ms = 1234;
    CHECK(fg::parse_event_json(fg::event_json(e), back) && back.after_ms == 1234);
}

TEST(since_last) {
    fg::IntentTracker t;
    CHECK(t.since_last(5000) == -1);
    t.mark(1000);
    t.mark(3000);
    CHECK(t.since_last(4200) == 1200);
    CHECK(t.since_last(2000) == 1000);  // a later action can't explain an earlier change
    t.mark(0xFFFFFF00u);
    CHECK(t.since_last(0x00000100u) == 0x200);  // across the 32-bit wrap
}

TEST(version_newer) {
    CHECK(fg::version_newer("v1.0.2", "1.0.1"));
    CHECK(fg::version_newer("1.10.0", "1.9.9"));
    CHECK(fg::version_newer("2.0.0", "1.99.99"));
    CHECK(!fg::version_newer("v1.0.1", "1.0.1"));
    CHECK(!fg::version_newer("1.0.0", "1.0.1"));
    CHECK(!fg::version_newer("v1.0.1-rc.1", "1.0.1"));
    CHECK(fg::version_newer("1.1", "1.0.9"));
    CHECK(!fg::version_newer("garbage", "0.0.1"));
}

TEST(add_to_config_file) {
    const auto dir = std::filesystem::temp_directory_path() / ("focusguard-cfg-" + std::to_string(fg::wall_ms()));
    const std::string path = (dir / "sub" / "focusguard.conf").string();
    std::string err;
    // A missing file (and folder) is created.
    CHECK(fg::add_to_config_file(path, true, "Discord", err));
    fg::Config c;
    CHECK(fg::load_config_file(path, c, err) == 0 && c.allow == std::vector<std::string>{"Discord"});
    // Comments survive; the entry moves off the other list; repeats change nothing.
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fputs("# mine\ngrace = 700\nblock = discord, Slack\n", f);
        std::fclose(f);
    }
    CHECK(fg::add_to_config_file(path, true, "Discord", err));
    CHECK(fg::add_to_config_file(path, true, "discord", err));
    c = fg::Config{};
    CHECK(fg::load_config_file(path, c, err) == 0);
    CHECK(c.grace_ms == 700 && c.block == std::vector<std::string>{"Slack"} && c.allow == std::vector<std::string>{"Discord"});
    std::string text;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        for (int ch; (ch = std::fgetc(f)) != EOF;) text += (char)ch;
        std::fclose(f);
    }
    CHECK(text.find("# mine") == 0);
    // A broken file is left alone.
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fputs("bogus = 1\n", f);
        std::fclose(f);
    }
    CHECK(!fg::add_to_config_file(path, true, "x", err) && !err.empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(event_log) {
    fg::EventLog log;
    for (int i = 0; i < 350; ++i) log.add({1000 + i, "blocked", "a", "", "", "", ""});
    const std::string j = log.json();
    CHECK(j.find("\"time\":1049") == std::string::npos);  // the oldest 50 dropped
    CHECK(j.find("\"time\":1050") != std::string::npos);
    log.clear();
    CHECK_EQ(log.json(), "{\"type\":\"events\",\"items\":[]}");
}

TEST(history) {
    const auto dir = std::filesystem::temp_directory_path() / ("focusguard-test-" + std::to_string(fg::wall_ms()));
    {
        fg::History h;
        fg::EventLog log;
        fg::Stats all;
        long long since = 0;
        CHECK(h.open(dir.string(), log, all, since));
        CHECK(since > 0 && all.total() == 0);
        h.append({5, "blocked", "Popup", "", "", "x", "no user action"});
        h.append({6, "allowed", "Editor", "", "", "y", "user action"});  // not stored
        all.record("Popup", 2);
        h.save_stats(all, 1234);
    }
    {
        fg::History h;
        fg::EventLog log;
        fg::Stats all;
        long long since = 0;
        CHECK(h.open(dir.string(), log, all, since));
        CHECK_EQ(since, 1234);
        CHECK(all.total() == 2 && all.ranked()[0].first == "Popup");
        const std::string j = log.json();
        CHECK(j.find("Popup") != std::string::npos && j.find("Editor") == std::string::npos);
        for (int i = 0; i < (int)fg::History::kCompact; ++i) h.append({100 + i, "blocked", "x", "", "", "", ""});
        h.clear_events();
    }
    {
        fg::History h;
        fg::EventLog log;
        fg::Stats all;
        long long since = 0;
        CHECK(h.open(dir.string(), log, all, since));
        CHECK_EQ(log.json(), "{\"type\":\"events\",\"items\":[]}");
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(history_compaction) {
    const auto dir = std::filesystem::temp_directory_path() / ("focusguard-test-c-" + std::to_string(fg::wall_ms()));
    fg::History h;
    fg::EventLog log;
    fg::Stats all;
    long long since = 0;
    CHECK(h.open(dir.string(), log, all, since));
    for (int i = 0; i < (int)fg::History::kCompact + 10; ++i) h.append({1 + i, "blocked", "x", "", "", "", ""});
    size_t lines = 0;
    if (FILE* f = std::fopen((dir / "events.jsonl").string().c_str(), "r")) {
        for (int ch; (ch = std::fgetc(f)) != EOF;) lines += ch == '\n';
        std::fclose(f);
    }
    CHECK(lines <= fg::History::kKeep + 10);  // compacted, not growing forever
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(control_commands) {
    fg::EventLog log;
    log.add({7, "blocked", "a", "", "", "", "r"});
    bool paused = false, reloaded = false, quit = false, cleared = false, reset = false;
    uint32_t minutes = 99;
    const fg::ControlHooks hooks{
        [] { return std::string("{\"type\":\"status\"}"); },
        [] { return fg::windows_json({{"App", "app", "Cls", "inst", "Title", true, false, true}}); },
        [&](bool p, uint32_t m) { paused = p, minutes = m; },
        [&] { reloaded = true; },
        [&] { quit = true; },
        [&] { cleared = true; },
        [&] { reset = true; },
    };
    bool sub = false;
    CHECK_EQ(fg::handle_control("status", log, hooks, sub), "{\"type\":\"status\"}\n");
    CHECK(fg::handle_control(" PAUSE \r", log, hooks, sub).find("\"ok\"") != std::string::npos && paused);
    CHECK(minutes == 0);
    fg::handle_control("resume", log, hooks, sub);
    CHECK(!paused);
    CHECK(fg::handle_control("pause 15", log, hooks, sub).find("\"ok\"") != std::string::npos);
    CHECK(paused && minutes == 15);
    paused = false;
    for (const char* bad : {"pause 0", "pause 1441", "pause x", "pause -5", "pause 15m"})
        CHECK(fg::handle_control(bad, log, hooks, sub).find("\"error\"") != std::string::npos);
    CHECK(!paused);
    fg::handle_control("reload", log, hooks, sub);
    fg::handle_control("quit", log, hooks, sub);
    fg::handle_control("clear-history", log, hooks, sub);
    fg::handle_control("reset-stats", log, hooks, sub);
    CHECK(reloaded && quit && cleared && reset);
    CHECK(fg::handle_control("events", log, hooks, sub).find("\"time\":7") != std::string::npos);
    CHECK(fg::handle_control("windows", log, hooks, sub).find("\"blocked\":true") != std::string::npos);
    CHECK(!sub);
    fg::handle_control("subscribe", log, hooks, sub);
    CHECK(sub);
    CHECK(fg::handle_control("nope", log, hooks, sub).find("\"error\"") != std::string::npos);
    CHECK(fg::handle_control("", log, hooks, sub).empty());
}

TEST(status_json) {
    fg::Config c;
    c.dry_run = true;
    fg::Stats session, all;
    session.record("Popup");
    all.record("Popup", 5);
    const std::string j =
        fg::status_json({c, true, 1793100000000LL, session, all, 42, 1234, "/x/focusguard.conf", true, {{"dry-run", true}}});
    for (const char* part : {"\"paused\":true", "\"paused_until\":1793100000000", "\"dry_run\":true", "\"blocked\":1", "\"blocked_total\":5",
                             "\"since\":42", "\"pid\":1234", "\"cli_overrides\":true",
                             "\"overrides\":[{\"key\":\"dry-run\",\"value\":true}]", "\"history\":true"})
        CHECK(j.find(part) != std::string::npos);
}

int main() {
    fg::log_state().level = -1;  // keep test output clean (option errors still go to stderr)
    for (const Test& t : tests()) {
        g_current = t.name;
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d failed checks\n", tests().size(), g_failures);
    return g_failures ? 1 : 0;
}
