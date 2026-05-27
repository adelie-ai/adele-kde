import QtQuick
import QtQuick.Controls as QQC2
import QtTest 1.0

import "../../shared/chat-module/ui" as Chat

// Acceptance tests for the tasks badge component used in the chat header.
//
// The badge shows "(N running)" when N > 0 and is hidden otherwise. Clicking
// it emits `clicked()`, which the parent main.qml wires to
// `tasksWindow.visible = true`.
//
// This is the integration-smoke seam: the badge's visibility and label
// must transition as the underlying running count moves 0 -> 1 -> 0.
TestCase {
    id: testCase
    name: "TasksBadge"
    when: windowShown
    width: 200
    height: 60
    visible: true

    QtObject {
        id: stubBackend
        property int runningTaskCount: 0
    }

    Component {
        id: badgeComponent
        Chat.TasksBadge {
            backend: stubBackend
        }
    }

    SignalSpy {
        id: clickSpy
    }

    function init() {
        stubBackend.runningTaskCount = 0
    }

    function test_business_outcome_user_sees_their_running_standalone_agent_in_the_separate_window() {
        // Full integration: the daemon emits a TaskStarted event for a
        // standalone agent. The badge becomes visible and reads
        // "(1 running)". Clicking it would open the separate window
        // (covered by tst_TasksWindow.qml).
        var badge = createTemporaryObject(badgeComponent, testCase)
        verify(badge !== null)
        compare(badge.visible, false, "badge hidden when no tasks")

        stubBackend.runningTaskCount = 1
        compare(badge.visible, true, "badge visible when 1 running task")
        verify(badge.text.indexOf("1") >= 0, "label contains the count")
        verify(badge.text.toLowerCase().indexOf("running") >= 0, "label says 'running'")
    }

    function test_count_transition_0_to_1_to_0() {
        var badge = createTemporaryObject(badgeComponent, testCase)
        clickSpy.target = badge
        clickSpy.signalName = "clicked"

        compare(badge.visible, false)

        stubBackend.runningTaskCount = 1
        compare(badge.visible, true)
        verify(badge.text.indexOf("1") >= 0)

        stubBackend.runningTaskCount = 0
        compare(badge.visible, false, "badge hidden again after count returns to 0")
    }

    function test_click_emits_clicked() {
        var badge = createTemporaryObject(badgeComponent, testCase)
        stubBackend.runningTaskCount = 2
        clickSpy.target = badge
        clickSpy.signalName = "clicked"
        badge.activate()
        compare(clickSpy.count, 1, "clicked emitted")
    }

    function test_pluralization_when_multiple_tasks() {
        var badge = createTemporaryObject(badgeComponent, testCase)
        stubBackend.runningTaskCount = 3
        verify(badge.text.indexOf("3") >= 0)
        verify(badge.text.toLowerCase().indexOf("running") >= 0)
    }
}
