import QtQuick
import QtTest 1.0

import "../../shared/chat-module/ui/ConversationList.js" as ConversationList

// Acceptance tests for the conversation-picker row helper.
//
// Background
// ----------
// The core sends the whole conversation inventory in the `conversations` view
// event, archived rows included, and each row carries `archived`. The picker
// offers active conversations only, so the rows are filtered here before they
// reach the model. The logic is a pure module so it can be unit-tested without
// instantiating ChatView (which needs Plasma QML modules that a generic
// qmltestrunner environment does not have).
//
// The helper exposes:
//   - ConversationList.isArchived(row)            -> bool
//   - ConversationList.activeConversations(rows)  -> array
//   - ConversationList.indexById(rows, id)        -> int (-1 = absent)
//   - ConversationList.titleById(rows, id)        -> string ("" = absent)
//
TestCase {
    id: testCase
    name: "ConversationList"

    function row(id, title, archived) {
        return { id: id, title: title, message_count: 2, archived: archived }
    }

    // A mixed inventory: the shape the core sends once it reports every
    // conversation. Active and archived rows arrive in one list, interleaved.
    function mixedInventory() {
        return [
            row("a1", "Active one", false),
            row("z1", "Archived one", true),
            row("a2", "Active two", false),
            row("z2", "Archived two", true)
        ]
    }

    // -- acceptance: an archived row is not an ordinary picker row ------------

    function test_an_archived_conversation_is_not_offered_in_the_picker() {
        var offered = ConversationList.activeConversations(mixedInventory())
        var ids = offered.map(function(c) { return c.id })
        compare(ids, ["a1", "a2"])
    }

    function test_an_active_conversation_stays_in_the_picker() {
        // The same list proves the filter removes archived rows only, so the
        // fix cannot pass by emptying the picker.
        var offered = ConversationList.activeConversations(mixedInventory())
        compare(offered.length, 2)
        compare(offered[0].title, "Active one")
        compare(offered[1].title, "Active two")
    }

    function test_the_filter_reads_the_archived_field_of_the_row() {
        // Same title, same message count: only `archived` separates the two, so
        // nothing but that field can be driving the decision.
        var rows = [row("x", "Same title", false), row("y", "Same title", true)]
        var offered = ConversationList.activeConversations(rows)
        compare(offered.length, 1)
        compare(offered[0].id, "x")
    }

    function test_the_filter_keeps_the_inventory_order() {
        var offered = ConversationList.activeConversations([
            row("c", "Third", false),
            row("a", "First", false),
            row("z", "Archived", true),
            row("b", "Second", false)
        ])
        compare(offered.map(function(c) { return c.id }), ["c", "a", "b"])
    }

    function test_an_all_archived_inventory_offers_nothing() {
        var offered = ConversationList.activeConversations([
            row("z1", "Archived one", true),
            row("z2", "Archived two", true)
        ])
        compare(offered.length, 0)
    }

    function test_the_filter_does_not_mutate_the_inventory() {
        // The raw list stays whole: the open-conversation lookup reads it, and
        // an Archived affordance would read it too.
        var rows = mixedInventory()
        ConversationList.activeConversations(rows)
        compare(rows.length, 4)
    }

    // -- isArchived: the field, and every way it can be missing --------------

    function test_isArchived_true_is_archived() {
        compare(ConversationList.isArchived(row("z", "Archived", true)), true)
    }

    function test_isArchived_false_is_active() {
        compare(ConversationList.isArchived(row("a", "Active", false)), false)
    }

    function test_isArchived_absent_field_is_active() {
        // An older core sends no `archived`; all of its conversations are
        // active, so the picker must list them as before.
        compare(ConversationList.isArchived({ id: "a", title: "Old core" }), false)
    }

    function test_isArchived_null_row_is_active() {
        compare(ConversationList.isArchived(null), false)
        compare(ConversationList.isArchived(undefined), false)
    }

    function test_isArchived_non_boolean_value_is_active() {
        // A value this build does not understand must never hide a
        // conversation: an unreachable conversation is worse than an extra row.
        compare(ConversationList.isArchived({ id: "a", archived: "yes" }), false)
        compare(ConversationList.isArchived({ id: "a", archived: 1 }), false)
    }

    function test_activeConversations_of_empty_is_empty() {
        compare(ConversationList.activeConversations([]).length, 0)
    }

    function test_activeConversations_of_null_is_empty() {
        // The event handler passes `data.items || []`, but the picker model must
        // never become undefined even if that changes.
        compare(ConversationList.activeConversations(null).length, 0)
        compare(ConversationList.activeConversations(undefined).length, 0)
    }

    // -- indexById: the picker's current-row lookup ---------------------------

    function test_indexById_finds_the_row() {
        compare(ConversationList.indexById(mixedInventory(), "a2"), 2)
    }

    function test_indexById_of_a_hidden_row_is_absent() {
        // The lookup the picker runs is over the FILTERED list, so an archived
        // conversation reports -1 there and the picker selects no row.
        var offered = ConversationList.activeConversations(mixedInventory())
        compare(ConversationList.indexById(offered, "z1"), -1)
    }

    function test_indexById_unknown_id_is_absent() {
        compare(ConversationList.indexById(mixedInventory(), "nope"), -1)
    }

    function test_indexById_empty_id_is_absent() {
        compare(ConversationList.indexById(mixedInventory(), ""), -1)
        compare(ConversationList.indexById(mixedInventory(), null), -1)
    }

    function test_indexById_empty_list_is_absent() {
        compare(ConversationList.indexById([], "a1"), -1)
        compare(ConversationList.indexById(null, "a1"), -1)
    }

    // -- titleById: naming the open conversation when it has no row ----------

    function test_titleById_finds_the_title() {
        compare(ConversationList.titleById(mixedInventory(), "z1"), "Archived one")
    }

    function test_titleById_of_an_archived_open_conversation_is_its_own_title() {
        // The daemon archives on a schedule and the core keeps the open
        // conversation open, so the open conversation can be archived. It has no
        // picker row, and the picker must name it rather than show the title of
        // whatever row happens to be selected.
        var inventory = mixedInventory()
        var offered = ConversationList.activeConversations(inventory)
        compare(ConversationList.indexById(offered, "z2"), -1)
        compare(ConversationList.titleById(inventory, "z2"), "Archived two")
    }

    function test_titleById_unknown_id_is_empty() {
        compare(ConversationList.titleById(mixedInventory(), "nope"), "")
    }

    function test_titleById_empty_id_is_empty() {
        compare(ConversationList.titleById(mixedInventory(), ""), "")
        compare(ConversationList.titleById(mixedInventory(), null), "")
    }

    function test_titleById_missing_title_is_empty() {
        compare(ConversationList.titleById([{ id: "a" }], "a"), "")
    }

    function test_titleById_empty_list_is_empty() {
        compare(ConversationList.titleById([], "a1"), "")
        compare(ConversationList.titleById(null, "a1"), "")
    }
}
