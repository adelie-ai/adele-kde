.pragma library

// Pure helpers for shelling out to the plasmoid's `dbus_client.py` helper
// (adele-kde#64 / KDE-11 / KDE-12). Factored out of ChatView.qml so the
// shell-safety and pending-callback bookkeeping can be unit-tested without
// instantiating the full ChatView (which depends on
// `org.kde.plasma.plasmoid` / `Plasma5Support` that aren't loadable from a
// generic qmltestrunner environment).

// Single-quote a value for a POSIX shell. Wrapping in single quotes and
// replacing each embedded `'` with `'\''` is injection-safe for arbitrary
// content (paths with spaces/quotes, user prompts, ids). EVERY argument that
// reaches a shell must go through this (KDE-11). null/undefined are coerced to
// the empty quoted string rather than throwing.
function shellEscape(value) {
    return "'" + String(value === undefined || value === null ? "" : value).replace(/'/g, "'\\''") + "'"
}

// Turn a `Qt.resolvedUrl(...).toString()` file URL into the real on-disk path.
// resolvedUrl percent-encodes the URL, so a path containing a space arrives as
// ".../My%20Widget/dbus_client.py"; passing that to python3 (even
// shell-escaped) points at a file that does not exist. Strip the `file://`
// prefix and decodeURIComponent so the actual path is recovered (KDE-11). The
// result is still expected to pass through shellEscape() before reaching a
// shell.
function decodeHelperPath(resolvedUrlString) {
    var raw = String(resolvedUrlString === undefined || resolvedUrlString === null ? "" : resolvedUrlString)
    if (raw.indexOf("file://") === 0) {
        raw = raw.substring("file://".length)
    }
    try {
        return decodeURIComponent(raw)
    } catch (e) {
        // Malformed percent-sequence: fall back to the raw (still-escaped)
        // value rather than throwing — better a wrong path than a crash.
        return raw
    }
}

// Drain every pending entry whose command equals `sourceName`, splicing them
// out of the four parallel arrays in place and returning the collected
// callbacks/flags.
//
// Plasma5Support keys a DataSource connection by its source string, so two
// in-flight runCommand() calls with an IDENTICAL command collapse into ONE
// connection and fire onNewData only once. Servicing just the first matching
// entry stranded every later duplicate forever (its callbacks never ran — a
// wedged refresh/poll). Draining all matches resolves every caller (KDE-12).
//
// The arrays are the GC-rooted QML `property var`s documented near the
// _pendingCmds declaration in ChatView.qml; we only splice them and push into
// fresh primitive/callback arrays, never building a dynamic-keyed object
// literal, so the QV4 insertMember GC hazard does not apply.
//
// Returns { successCbs, errorCbs, debugFlags } (parallel arrays, in push
// order; empty when nothing matched).
function drainPending(pendingCmds, pendingSuccess, pendingError, pendingDebug, sourceName) {
    var successCbs = []
    var errorCbs = []
    var debugFlags = []
    for (var i = pendingCmds.length - 1; i >= 0; i--) {
        if (pendingCmds[i] !== sourceName) {
            continue
        }
        successCbs.push(pendingSuccess[i])
        errorCbs.push(pendingError[i])
        debugFlags.push(pendingDebug[i])
        pendingCmds.splice(i, 1)
        pendingSuccess.splice(i, 1)
        pendingError.splice(i, 1)
        pendingDebug.splice(i, 1)
    }
    return { successCbs: successCbs, errorCbs: errorCbs, debugFlags: debugFlags }
}
