import QtQuick
import QtTest 1.0

import "../../shared/chat-module/ui/HelperRunner.js" as HelperRunner

// Acceptance tests for the helper-runner shell-safety + pending-callback
// bookkeeping factored out of ChatView.qml (adele-kde#64 — KDE-11/KDE-12).
//
// Background
// ----------
// The plasmoid shells out to `dbus_client.py` via Plasma5Support.DataSource.
// Two failure modes motivated this extraction:
//
//   * KDE-11 (shell-injection-adjacent): the helper path was percent-encoded
//     (Qt.resolvedUrl) and one caller concatenated bare single quotes instead
//     of shell-escaping. A path with a space ("My Widget") arrived as
//     "My%20Widget" → wrong file; a path with a quote could break out of the
//     command. Every argument must go through shellEscape, and the path must be
//     decodeURIComponent'd first.
//
//   * KDE-12 (pending-callback stranding): Plasma5Support keys a DataSource
//     connection by its source string, so two identical in-flight commands
//     collapse into ONE connection and fire onNewData once. Servicing only the
//     first pending entry stranded the rest forever. drainPending services all
//     matches.
//
// These are pure JS, so they're tested directly without instantiating ChatView
// (which needs org.kde.plasma.plasmoid / Plasma5Support).
TestCase {
    id: testCase
    name: "HelperRunner"

    // ── shellEscape (KDE-11) ─────────────────────────────────────────────

    function test_shellEscape_plain() {
        compare(HelperRunner.shellEscape("hello"), "'hello'")
    }

    function test_shellEscape_with_spaces() {
        compare(HelperRunner.shellEscape("/home/me/My Widget/dbus_client.py"),
                "'/home/me/My Widget/dbus_client.py'")
    }

    function test_shellEscape_with_single_quote() {
        // The classic break-out attempt: a single quote must be neutralised as
        // '\'' so it can never close the quoting and start a new shell word.
        compare(HelperRunner.shellEscape("a'b"), "'a'\\''b'")
    }

    function test_shellEscape_injection_attempt() {
        // "'; rm -rf ~; echo '" must remain a single inert argument.
        var escaped = HelperRunner.shellEscape("'; rm -rf ~; echo '")
        // Wrapped in single quotes, every embedded quote rewritten — no bare
        // quote survives to terminate the quoting.
        verify(escaped.charAt(0) === "'")
        verify(escaped.charAt(escaped.length - 1) === "'")
        compare(escaped, "''\\''; rm -rf ~; echo '\\'''")
    }

    function test_shellEscape_null_and_undefined_safe() {
        compare(HelperRunner.shellEscape(null), "''")
        compare(HelperRunner.shellEscape(undefined), "''")
    }

    function test_shellEscape_number_coerced() {
        compare(HelperRunner.shellEscape(42), "'42'")
    }

    // ── decodeHelperPath (KDE-11) ────────────────────────────────────────

    function test_decodeHelperPath_strips_file_scheme() {
        compare(HelperRunner.decodeHelperPath("file:///home/me/x/dbus_client.py"),
                "/home/me/x/dbus_client.py")
    }

    function test_decodeHelperPath_decodes_spaces() {
        // The KDE-11 bug: %20 must come back as a real space so the path exists.
        compare(HelperRunner.decodeHelperPath("file:///home/me/My%20Widget/dbus_client.py"),
                "/home/me/My Widget/dbus_client.py")
    }

    function test_decodeHelperPath_decodes_other_escapes() {
        // Parentheses / quotes can be percent-encoded by resolvedUrl too.
        compare(HelperRunner.decodeHelperPath("file:///opt/a%28b%29/dbus_client.py"),
                "/opt/a(b)/dbus_client.py")
    }

    function test_decodeHelperPath_no_scheme_passthrough() {
        compare(HelperRunner.decodeHelperPath("/already/plain/path.py"),
                "/already/plain/path.py")
    }

    function test_decodeHelperPath_malformed_percent_does_not_throw() {
        // A lone "%" is an invalid escape; decodeURIComponent would throw.
        // The helper must fall back to the raw value rather than crash.
        var out = HelperRunner.decodeHelperPath("file:///bad/%/path.py")
        compare(out, "/bad/%/path.py")
    }

    function test_decodeHelperPath_null_safe() {
        compare(HelperRunner.decodeHelperPath(null), "")
        compare(HelperRunner.decodeHelperPath(undefined), "")
    }

    function test_business_outcome_spaced_path_is_runnable() {
        // End-to-end: resolvedUrl-style encoded path → decoded → escaped yields
        // a command word that points at the REAL file and is shell-safe.
        var resolved = "file:///home/me/My%20Widget/code/dbus_client.py"
        var decoded = HelperRunner.decodeHelperPath(resolved)
        compare(decoded, "/home/me/My Widget/code/dbus_client.py")
        var word = "python3 " + HelperRunner.shellEscape(decoded)
        compare(word, "python3 '/home/me/My Widget/code/dbus_client.py'")
    }

    // ── drainPending (KDE-12) ────────────────────────────────────────────

    function _push(cmds, succ, err, dbg, cmd, s, e, d) {
        cmds.push(cmd); succ.push(s); err.push(e); dbg.push(d)
    }

    function test_drainPending_single_match() {
        var cmds = [], succ = [], err = [], dbg = []
        var called = []
        _push(cmds, succ, err, dbg, "cmd-a",
              function(o) { called.push("a:" + o) },
              function(x) { called.push("a-err") }, true)

        var r = HelperRunner.drainPending(cmds, succ, err, dbg, "cmd-a")
        compare(r.successCbs.length, 1)
        compare(r.debugFlags[0], true)
        // The entry was spliced out.
        compare(cmds.length, 0)
        r.successCbs[0]("ok")
        compare(called, ["a:ok"])
    }

    function test_drainPending_no_match_leaves_arrays_intact() {
        var cmds = ["cmd-a"], succ = [function(){}], err = [function(){}], dbg = [false]
        var r = HelperRunner.drainPending(cmds, succ, err, dbg, "cmd-z")
        compare(r.successCbs.length, 0)
        compare(cmds.length, 1, "non-matching entry must remain pending")
        compare(cmds[0], "cmd-a")
    }

    function test_drainPending_duplicate_commands_all_serviced() {
        // The KDE-12 core case: two identical commands queued; onNewData fires
        // once for the shared source; BOTH callbacks must resolve so neither
        // caller is stranded.
        var cmds = [], succ = [], err = [], dbg = []
        var resolved = []
        _push(cmds, succ, err, dbg, "tasks-list",
              function(o) { resolved.push("first") }, function(){}, false)
        _push(cmds, succ, err, dbg, "tasks-list",
              function(o) { resolved.push("second") }, function(){}, false)

        var r = HelperRunner.drainPending(cmds, succ, err, dbg, "tasks-list")
        compare(r.successCbs.length, 2, "both duplicate entries drained")
        compare(cmds.length, 0, "no pending entry stranded")
        for (var i = 0; i < r.successCbs.length; i++) {
            r.successCbs[i]("ok")
        }
        compare(resolved.length, 2, "both callers' callbacks fired")
    }

    function test_drainPending_only_matching_command_removed() {
        // Interleaved distinct commands: draining one must not disturb the
        // other's pending entry or its callbacks.
        var cmds = [], succ = [], err = [], dbg = []
        var fired = []
        _push(cmds, succ, err, dbg, "cmd-a", function(){ fired.push("a") }, function(){}, false)
        _push(cmds, succ, err, dbg, "cmd-b", function(){ fired.push("b") }, function(){}, false)
        _push(cmds, succ, err, dbg, "cmd-a", function(){ fired.push("a2") }, function(){}, false)

        var r = HelperRunner.drainPending(cmds, succ, err, dbg, "cmd-a")
        compare(r.successCbs.length, 2)
        compare(cmds.length, 1, "cmd-b must remain pending")
        compare(cmds[0], "cmd-b")
        for (var i = 0; i < r.successCbs.length; i++) r.successCbs[i]("ok")
        // Two cmd-a callbacks fired (push order: index 0 then index 2 collected
        // last-to-first, so "a2" then "a"); cmd-b untouched.
        compare(fired.indexOf("b"), -1, "cmd-b callback must not fire")
        compare(fired.length, 2)
    }

    function test_drainPending_preserves_parallel_alignment() {
        // The four arrays must stay index-aligned after a splice from the
        // middle: drain cmd-b, then the remaining a/c entries must still pair
        // their own success/error/debug.
        var cmds = ["a", "b", "c"]
        var succ = ["sa", "sb", "sc"]
        var err = ["ea", "eb", "ec"]
        var dbg = [1, 2, 3]
        HelperRunner.drainPending(cmds, succ, err, dbg, "b")
        compare(cmds, ["a", "c"])
        compare(succ, ["sa", "sc"])
        compare(err, ["ea", "ec"])
        compare(dbg, [1, 3])
    }
}
