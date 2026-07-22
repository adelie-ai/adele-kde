import QtQuick
import QtTest 1.0

import "../../shared/chat-module/ui/QueueRecall.js" as QueueRecall

// Acceptance tests for the message-queue recall/preview helper.
//
// Background
// ----------
// The composer stays editable while a reply streams; submitting enqueues the
// text. Up-arrow (when the composer is empty) recalls a queued message into the
// composer to edit, walking backward from the last; Down while editing walks
// forward or cancels the edit. The index arithmetic is a pure function so it
// can be unit-tested here without instantiating ChatView (which depends on
// Plasma QML modules that aren't loadable from a generic qmltestrunner env).
//
// The helper exposes:
//   - QueueRecall.normalizeEditing(value)             -> int   (-1 = not editing)
//   - QueueRecall.recallDecision(dir, empty, edit, n) -> { action, index }
//   - QueueRecall.previewText(text)                   -> string
//
TestCase {
    id: testCase
    name: "QueueRecall"

    // ── normalizeEditing: the queued_messages `editing` field → plain index ──

    function test_normalizeEditing_null_is_not_editing() {
        compare(QueueRecall.normalizeEditing(null), -1)
    }

    function test_normalizeEditing_undefined_is_not_editing() {
        compare(QueueRecall.normalizeEditing(undefined), -1)
    }

    function test_normalizeEditing_negative_is_not_editing() {
        compare(QueueRecall.normalizeEditing(-4), -1)
    }

    function test_normalizeEditing_zero_is_index_zero() {
        // Index 0 is a real editing target, distinct from "not editing" (-1).
        compare(QueueRecall.normalizeEditing(0), 0)
    }

    function test_normalizeEditing_positive_passes_through() {
        compare(QueueRecall.normalizeEditing(3), 3)
    }

    function test_normalizeEditing_float_floors() {
        compare(QueueRecall.normalizeEditing(2.9), 2)
    }

    function test_normalizeEditing_non_numeric_is_not_editing() {
        compare(QueueRecall.normalizeEditing("nope"), -1)
    }

    // ── recallDecision: Up ───────────────────────────────────────────────────

    function test_up_empty_queue_does_nothing() {
        compare(QueueRecall.recallDecision("up", true, -1, 0).action, "none")
    }

    function test_up_not_editing_recalls_last() {
        var d = QueueRecall.recallDecision("up", true, -1, 3)
        compare(d.action, "edit")
        compare(d.index, 2)
    }

    function test_up_editing_walks_backward() {
        var d = QueueRecall.recallDecision("up", true, 2, 3)
        compare(d.action, "edit")
        compare(d.index, 1)
    }

    function test_up_at_first_item_does_nothing() {
        // Already editing index 0: Up must not wrap past the first item.
        compare(QueueRecall.recallDecision("up", true, 0, 3).action, "none")
    }

    function test_up_nonempty_field_is_caret_movement() {
        // A non-empty composer keeps Up as ordinary caret movement.
        compare(QueueRecall.recallDecision("up", false, -1, 3).action, "none")
    }

    // ── recallDecision: Down ─────────────────────────────────────────────────

    function test_down_not_editing_does_nothing() {
        // Down only walks the queue while an edit is checked out.
        compare(QueueRecall.recallDecision("down", true, -1, 3).action, "none")
    }

    function test_down_editing_walks_forward() {
        var d = QueueRecall.recallDecision("down", true, 0, 3)
        compare(d.action, "edit")
        compare(d.index, 1)
    }

    function test_down_past_last_cancels_edit() {
        // Down past the final queued item abandons the edit.
        compare(QueueRecall.recallDecision("down", true, 2, 3).action, "cancel")
    }

    function test_down_nonempty_field_is_caret_movement() {
        compare(QueueRecall.recallDecision("down", false, 0, 3).action, "none")
    }

    // ── recallDecision: misc ─────────────────────────────────────────────────

    function test_unknown_direction_does_nothing() {
        compare(QueueRecall.recallDecision("left", true, -1, 3).action, "none")
    }

    function test_up_editing_with_empty_queue_does_nothing() {
        compare(QueueRecall.recallDecision("up", true, 0, 0).action, "none")
    }

    // ── previewText ──────────────────────────────────────────────────────────

    function test_previewText_collapses_newlines_and_whitespace() {
        compare(QueueRecall.previewText("hello\n\n  world\ttab"), "hello world tab")
    }

    function test_previewText_trims() {
        compare(QueueRecall.previewText("   padded   "), "padded")
    }

    function test_previewText_null_safe() {
        compare(QueueRecall.previewText(null), "")
        compare(QueueRecall.previewText(undefined), "")
    }
}
