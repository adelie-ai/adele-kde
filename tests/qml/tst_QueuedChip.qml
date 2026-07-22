import QtQuick
import QtTest 1.0

import "../../shared/chat-module/ui" as Chat

// Acceptance test for the queued-chip click->index wiring (AC11).
//
// Background
// ----------
// The message-queue chips show the VISIBLE queue (the snapshot omits any item
// checked out for editing). Clicking a chip must recall the RIGHT queued
// message: while another item is checked out, the visible index no longer
// equals the full-queue index the reducer's EditQueued expects, so the click
// is translated through QueueRecall.chipEditIndex before it reaches the core.
//
// That translation was the fix in commit 77a839a; re-inlining
// `core.editQueued(index)` (skipping the mapping) would silently edit the wrong
// message and still ship green. This test pins the wiring: it drives a REAL
// click on the chip's edit affordance and asserts the emitted full-queue index.
//
// The chip lives in its own component (QueuedChip.qml) precisely so it can be
// instantiated headless here — ChatView itself imports the Plasmoid and
// native-client modules and can't be loaded in a generic qmltestrunner env.
TestCase {
    id: testCase
    name: "QueuedChip"
    when: windowShown
    width: 320
    height: 80
    visible: true

    Component {
        id: chipComponent
        Chat.QueuedChip {}
    }

    SignalSpy {
        id: editSpy
    }

    SignalSpy {
        id: removeSpy
    }

    // A single SignalSpy is reused across cases; reset its tally between tests
    // so one test's emissions can't bleed into the next's count.
    function init() {
        editSpy.clear()
        removeSpy.clear()
    }

    // AC11: full queue [A,B,C,D], editing B (full idx 1) → visible list [A,C,D].
    // A click on visible index 1 (C) must be reported as full index 2, NOT the
    // raw visible index 1 (which would edit B, the wrong message).
    function test_chip_click_maps_visible_index_through_chipEditIndex() {
        var chip = createTemporaryObject(chipComponent, testCase, {
            chipIndex: 1,
            chipText: "third prompt",
            editingIndex: 1,
        })
        verify(chip !== null)
        editSpy.target = chip
        editSpy.signalName = "editRequested"

        var editArea = findChild(chip, "queuedChipEditArea")
        verify(editArea !== null, "chip exposes its edit MouseArea")
        mouseClick(editArea)

        compare(editSpy.count, 1, "editRequested emitted once")
        compare(editSpy.signalArguments[0][0], 2,
            "visible index 1 with editing 1 maps to full index 2")
    }

    // Not editing (editingIndex -1): the visible and full indices coincide, so a
    // click on visible index 2 maps straight through to full index 2.
    function test_chip_click_is_identity_when_not_editing() {
        var chip = createTemporaryObject(chipComponent, testCase, {
            chipIndex: 2,
            chipText: "some prompt",
            editingIndex: -1,
        })
        verify(chip !== null)
        editSpy.target = chip
        editSpy.signalName = "editRequested"

        var editArea = findChild(chip, "queuedChipEditArea")
        verify(editArea !== null)
        mouseClick(editArea)

        compare(editSpy.count, 1)
        compare(editSpy.signalArguments[0][0], 2,
            "not editing: visible index maps to itself")
    }

    // The ✕ removes by VISIBLE index (what RemoveQueued expects), unmapped.
    function test_chip_remove_reports_visible_index() {
        var chip = createTemporaryObject(chipComponent, testCase, {
            chipIndex: 3,
            chipText: "drop me",
            editingIndex: 1,
        })
        verify(chip !== null)
        removeSpy.target = chip
        removeSpy.signalName = "removeRequested"

        var removeButton = findChild(chip, "queuedChipRemoveButton")
        verify(removeButton !== null, "chip exposes its remove button")
        mouseClick(removeButton)

        compare(removeSpy.count, 1, "removeRequested emitted once")
        compare(removeSpy.signalArguments[0][0], 3,
            "remove reports the raw visible index")
    }
}
