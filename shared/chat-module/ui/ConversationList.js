// ConversationList.js - which conversations the picker offers, and what it names.
//
// Background
// ----------
// The core's `conversations` view-event carries the whole inventory: every
// conversation the account holds, archived rows together with active ones, each
// row flagged `archived` (client-ui-common's ffi/src/view_event.rs). The wider
// population is deliberate - it lets each client decide for itself whether to
// group archived rows, hide them, or list them inline, without a second query.
//
// The plasmoid has no archive affordance of its own, so it offers active
// conversations only. Archived rows are still worth keeping in hand: the daemon
// archives on a schedule (the KCM setting "Archive conversations after (days)"),
// and the core keeps a conversation open when it is archived under the reader.
// The open conversation can therefore be archived and have no row in the picker,
// which is why the title lookup below reads the raw inventory rather than the
// offered rows.
//
// Factored as a pure module so it is:
//   * Unit-testable from qmltestrunner without instantiating the full ChatView
//     (which depends on Plasma QML modules unavailable in a generic test env).
//   * Shared byte-identically by both plasmoid copies of ChatView.qml (the
//     `just chatview-sync` mirror keeps the files in lockstep).
//
// Public API
// ----------
//   isArchived(row) -> bool
//       Whether a row is filed away. Only a real `true` counts.
//
//   activeConversations(rows) -> array
//       The rows the picker offers, in inventory order. Never null.
//
//   indexById(rows, id) -> int
//       The position of a conversation in a list, or -1 when it is not there.
//
//   titleById(rows, id) -> string
//       The title a conversation carries in a list, or "" when it is not there.
//
.pragma library

// Whether the core has flagged this row archived.
//
// Only a literal `true` counts. A missing field (an older core sends none, and
// all of its conversations are active) and any value this build does not
// understand both read as active, because the failure that matters is a
// conversation the user can no longer reach - an extra row is recoverable, a
// hidden one is not.
function isArchived(row) {
    return !!row && row.archived === true
}

// The conversations the picker offers: the active rows, in inventory order.
//
// The argument is left untouched - the caller keeps the raw inventory, which is
// what `titleById` reads and what an Archived section would read.
function activeConversations(rows) {
    if (rows === null || rows === undefined) {
        return []
    }
    var offered = []
    for (var i = 0; i < rows.length; i++) {
        if (!isArchived(rows[i])) {
            offered.push(rows[i])
        }
    }
    return offered
}

// The position of a conversation within a list, or -1 when the list has no such
// row. Callers pass the OFFERED rows when they want a picker index, and the raw
// inventory when they want to know whether the core still holds a conversation.
function indexById(rows, id) {
    var target = String(id === null || id === undefined ? "" : id)
    if (target.length === 0 || rows === null || rows === undefined) {
        return -1
    }
    for (var i = 0; i < rows.length; i++) {
        if (rows[i] && rows[i].id === target) {
            return i
        }
    }
    return -1
}

// The title a conversation carries in a list, or "" when the list has no such
// row (and for a row that carries no title).
function titleById(rows, id) {
    var at = indexById(rows, id)
    if (at < 0) {
        return ""
    }
    var title = rows[at].title
    return (title === null || title === undefined) ? "" : String(title)
}
