import QtQuick
import QtTest 1.0

import "../../shared/chat-module/ui/MessageKind.js" as MessageKind

// Acceptance tests for the inline-note presentation marker.
//
// Background
// ----------
// A `say_this` line the model produces mid-turn reaches the widget as the
// core's `inline_note` view-event. It used to arrive with its kind already
// stringified into the text ("Spoken: …" / "(speech mode disabled) …") — the
// core did the presentation. The event now carries the kind as a structured
// token (`normal` / `spoken` / `speech_disabled`) with the text unmarked, and
// the marker is rendered HERE, at the presentation layer, where a client with a
// richer affordance (adele-mac's badge) can do something else with the same
// metadata.
//
// The plasmoid has no badge affordance, so the marker text IS its Spoken /
// speech-disabled indicator: these tests pin the rendered strings byte-for-byte
// against what the core used to emit, so the change is invisible on screen.
//
// The helper is a pure `.pragma library` module so it can be unit-tested here
// without instantiating ChatView (which needs Plasma QML modules that aren't
// loadable from a generic qmltestrunner env), and so both plasmoid copies of
// ChatView.qml share it byte-identically.
TestCase {
    id: testCase
    name: "MessageKind"

    // ── markerFor: the token → the prefix the widget shows ───────────────────

    function test_spoken_marker_matches_the_retired_core_wording() {
        compare(MessageKind.markerFor("spoken"), "Spoken: ")
    }

    function test_speech_disabled_marker_matches_the_retired_core_wording() {
        compare(MessageKind.markerFor("speech_disabled"), "(speech mode disabled) ")
    }

    function test_normal_has_no_marker() {
        compare(MessageKind.markerFor("normal"), "")
    }

    // Forward-compat: a kind this widget predates must render as an ordinary
    // note rather than as the literal token, and an older core that sends no
    // `kind` at all must keep working.
    function test_unknown_kind_has_no_marker() {
        compare(MessageKind.markerFor("whispered"), "")
    }

    function test_missing_kind_has_no_marker() {
        compare(MessageKind.markerFor(""), "")
        compare(MessageKind.markerFor(null), "")
        compare(MessageKind.markerFor(undefined), "")
    }

    // ── decorate: what ChatView actually appends to the transcript ───────────

    function test_decorate_prefixes_a_spoken_line() {
        compare(MessageKind.decorate("hello there", "spoken"), "Spoken: hello there")
    }

    function test_decorate_prefixes_a_suppressed_line() {
        compare(
            MessageKind.decorate("hello there", "speech_disabled"),
            "(speech mode disabled) hello there")
    }

    function test_decorate_leaves_an_ordinary_note_alone() {
        compare(MessageKind.decorate("Reconnected to the daemon.", "normal"),
                "Reconnected to the daemon.")
    }

    // A note whose own text happens to open with the marker wording must not be
    // touched — the whole point of the structured token is that prose is no
    // longer load-bearing.
    function test_decorate_does_not_double_mark_lookalike_prose() {
        compare(MessageKind.decorate("Spoken words are cheap", "normal"),
                "Spoken words are cheap")
    }

    function test_decorate_is_null_safe() {
        compare(MessageKind.decorate(null, "spoken"), "Spoken: ")
        compare(MessageKind.decorate(undefined, null), "")
        compare(MessageKind.decorate("", "speech_disabled"), "(speech mode disabled) ")
    }
}
