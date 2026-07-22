// QueueRecall.js — pure index/preview logic for the message-queuing composer.
//
// Background
// ----------
// The composer stays editable while a reply streams; submitting a prompt while
// busy enqueues it (the reducer, in the Rust core, owns the queue). The queued
// messages surface as chips, and the keyboard can recall one back into the
// composer to edit:
//   • Up  — when the composer is EMPTY: recall a queued message, walking
//           backward from the last toward the first. A non-empty composer keeps
//           Up as ordinary caret movement.
//   • Down — while an edit is checked out and the composer is empty: walk to the
//            next queued message, or cancel the edit past the last one.
//
// The index arithmetic is factored here as a pure module so it is:
//   • Unit-testable from qmltestrunner without instantiating the full ChatView
//     (which depends on Plasma QML modules unavailable in a generic test env).
//   • Shared byte-identically by both plasmoid copies of ChatView.qml (the
//     `just chatview-sync` mirror keeps the files in lockstep).
//
// Public API
// ----------
//   normalizeEditing(value) -> int
//       Map the queued_messages event's `editing` field to a plain index.
//       null / undefined / non-numeric / negative -> -1 ("not editing"), so the
//       caller can tell "not editing" apart from editing index 0.
//
//   recallDecision(direction, fieldEmpty, editing, count)
//       -> { action: "edit", index } | { action: "cancel" } | { action: "none" }
//       Decide what a Up/Down keypress should do. "none" means let the key act
//       normally (caret movement).
//
//   chipEditIndex(visible, editing) -> int
//       Translate a visible chip index to the full-queue index EditQueued wants
//       (the checked-out message is absent from the visible list but reinserted
//       before indexing). Identity when not editing.
//
//   previewText(text) -> string
//       Collapse whitespace (incl. newlines) to single spaces and trim, for a
//       compact one-line chip label. null / undefined -> "".
//
.pragma library

// Map the `editing` field of a queued_messages event to a plain index, or -1
// when no item is checked out for editing.
function normalizeEditing(value) {
    if (value === null || value === undefined) {
        return -1
    }
    var n = Number(value)
    if (!isFinite(n) || n < 0) {
        return -1
    }
    return Math.floor(n)
}

// Decide the effect of an Up/Down keypress on the queue.
//
//   direction  — "up" | "down"
//   fieldEmpty — true iff the composer text is empty
//   editing    — index currently checked out for editing, or -1
//   count      — number of queued messages (submit order)
function recallDecision(direction, fieldEmpty, editing, count) {
    var n = Number(count)
    if (!isFinite(n) || n <= 0) {
        return { action: "none" }
    }
    var edit = normalizeEditing(editing)

    if (direction === "up") {
        // A non-empty composer keeps Up as caret movement (don't fight it).
        if (!fieldEmpty) {
            return { action: "none" }
        }
        // Not editing: recall the last queued item. Editing: walk one earlier.
        var up = (edit < 0) ? (n - 1) : (edit - 1)
        if (up < 0) {
            return { action: "none" }
        }
        return { action: "edit", index: up }
    }

    if (direction === "down") {
        // Down only walks the queue while an edit is checked out, and only when
        // the composer is empty so it doesn't fight caret movement.
        if (edit < 0 || !fieldEmpty) {
            return { action: "none" }
        }
        // While editing, `count` is the VISIBLE snapshot — the checked-out item
        // is absent from it — so the full queue has `count + 1` slots and its
        // last full index is `count`. `edit` is a full index (EditQueued
        // reinserts before indexing), so walk to `edit + 1` while it still lands
        // on a real item, and cancel only once past the last (`edit + 1 > n`).
        var down = edit + 1
        if (down <= n) {
            return { action: "edit", index: down }
        }
        return { action: "cancel" }
    }

    return { action: "none" }
}

// Map a visible chip index to the full-queue index the reducer's EditQueued
// expects. While a message is checked out for editing it is ABSENT from the
// queued_messages event's `messages` array (it lives in the composer), yet the
// reducer reinserts it at its original slot BEFORE indexing. So a visible chip
// at or after the checked-out slot is one position lower than its full-queue
// index. When nothing is being edited (`editing` < 0) the two coincide.
//
//   visible — the chip's index within the visible (event) list
//   editing — the normalized editing index, or -1 when not editing
function chipEditIndex(visible, editing) {
    return (editing >= 0 && visible >= editing) ? visible + 1 : visible
}

// Collapse whitespace to single spaces and trim, for a compact chip label. The
// chip itself elides for width; this only flattens multi-line drafts.
function previewText(text) {
    if (text === null || text === undefined) {
        return ""
    }
    return String(text).replace(/\s+/g, " ").trim()
}
