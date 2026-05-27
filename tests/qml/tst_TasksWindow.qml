import QtQuick
import QtQuick.Window
import QtTest 1.0

import "../../shared/chat-module/ui" as Chat

// Acceptance tests for TasksWindow.qml — the separate, hidden-by-default
// process-manager window that the badge toggles visible.
TestCase {
    id: testCase
    name: "TasksWindow"
    when: windowShown

    QtObject {
        id: stubBackend
        property var tasks: []
        property var lastClosed: false
        function cancelTask(_id) {}
        function openConversation(_id) {}
        function refreshLogs(_id) {}
        function taskLogs(_id) { return "" }
    }

    Component {
        id: windowComponent
        Chat.TasksWindow {
            backend: stubBackend
            visible: false
        }
    }

    function test_window_visibility_toggles() {
        var win = createTemporaryObject(windowComponent, testCase)
        verify(win !== null)
        compare(win.visible, false, "starts hidden")
        win.visible = true
        compare(win.visible, true, "becomes visible when set")
    }

    function test_close_button_hides_window() {
        var win = createTemporaryObject(windowComponent, testCase)
        win.visible = true
        var closeButton = findChild(win, "tasksWindowCloseButton")
        verify(closeButton !== null, "close button found")
        closeButton.clicked()
        compare(win.visible, false, "close button hides the window")
    }

    function test_window_contains_a_tasks_view() {
        var win = createTemporaryObject(windowComponent, testCase)
        win.visible = true
        var tasksList = findChild(win, "tasksListView")
        verify(tasksList !== null, "TasksView is embedded in TasksWindow")
    }
}
