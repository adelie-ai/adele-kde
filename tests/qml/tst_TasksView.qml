import QtQuick
import QtQuick.Controls as QQC2
import QtTest 1.0

import "../../shared/chat-module/ui" as Chat

// Acceptance tests for the TasksView component (#7).
//
// `TasksView.qml` is a list-of-tasks + detail (log + buttons) view that
// drives off a backend object exposing:
//   - tasks: array of {id, title, status, ended_at, progress_hint}
//   - cancelTask(id), openConversation(id), refreshLogs(id)
//   - taskLogs(id) -> string
//
// The backend is dependency-injected so the test can substitute a stub.
TestCase {
    id: testCase
    name: "TasksView"
    when: windowShown
    width: 700
    height: 500
    visible: true

    QtObject {
        id: stubBackend
        property var tasks: []
        property var lastCancelledId: ""
        property var lastOpenedConversationId: ""
        property var lastRefreshedLogsId: ""
        property var logsByTask: ({})
        function cancelTask(taskId) {
            lastCancelledId = taskId
        }
        function openConversation(taskId) {
            lastOpenedConversationId = taskId
        }
        function refreshLogs(taskId) {
            lastRefreshedLogsId = taskId
        }
        function taskLogs(taskId) {
            return logsByTask[taskId] || ""
        }
    }

    Component {
        id: tasksViewComponent
        Chat.TasksView {
            anchors.fill: parent
            backend: stubBackend
        }
    }

    SignalSpy {
        id: viewLoadedSpy
        signalName: "completed"
    }

    function init() {
        stubBackend.tasks = []
        stubBackend.lastCancelledId = ""
        stubBackend.lastOpenedConversationId = ""
        stubBackend.lastRefreshedLogsId = ""
        stubBackend.logsByTask = ({})
    }

    function test_renders_three_fixture_rows() {
        stubBackend.tasks = [
            { id: "t-1", title: "Researcher", status: "running" },
            { id: "t-2", title: "Watcher",    status: "running" },
            { id: "t-3", title: "Done",       status: "completed", ended_at: 1717000010000 },
        ]
        var view = createTemporaryObject(tasksViewComponent, testCase)
        verify(view !== null, "TasksView created")
        var list = findChild(view, "tasksListView")
        verify(list !== null, "tasksListView found")
        compare(list.count, 3, "list renders 3 tasks")
    }

    function test_selecting_a_row_updates_selection_and_loads_logs() {
        stubBackend.tasks = [
            { id: "t-1", title: "Researcher", status: "running" },
            { id: "t-2", title: "Watcher",    status: "running" },
        ]
        stubBackend.logsByTask = ({ "t-1": "log-line-a\nlog-line-b" })
        var view = createTemporaryObject(tasksViewComponent, testCase)
        var list = findChild(view, "tasksListView")
        list.currentIndex = 0
        // Force the view to apply selection synchronously.
        view.selectedTaskId = stubBackend.tasks[0].id
        compare(view.selectedTaskId, "t-1")
        var logsArea = findChild(view, "taskLogsArea")
        verify(logsArea !== null, "taskLogsArea found")
        // The view should request logs for the selected task on selection.
        compare(stubBackend.lastRefreshedLogsId, "t-1")
    }

    function test_cancel_button_invokes_backend_cancelTask_with_selected_id() {
        stubBackend.tasks = [
            { id: "t-1", title: "Researcher", status: "running" },
        ]
        var view = createTemporaryObject(tasksViewComponent, testCase)
        view.selectedTaskId = "t-1"
        var cancelButton = findChild(view, "taskCancelButton")
        verify(cancelButton !== null, "cancel button found")
        cancelButton.clicked()
        compare(stubBackend.lastCancelledId, "t-1")
    }

    function test_open_conversation_button_invokes_backend() {
        stubBackend.tasks = [
            { id: "t-1", title: "Researcher", status: "running" },
        ]
        var view = createTemporaryObject(tasksViewComponent, testCase)
        view.selectedTaskId = "t-1"
        var openButton = findChild(view, "taskOpenConversationButton")
        verify(openButton !== null, "open conversation button found")
        openButton.clicked()
        compare(stubBackend.lastOpenedConversationId, "t-1")
    }

    function test_cancel_button_disabled_when_task_not_running() {
        stubBackend.tasks = [
            { id: "t-1", title: "Done", status: "completed", ended_at: 1 },
        ]
        var view = createTemporaryObject(tasksViewComponent, testCase)
        view.selectedTaskId = "t-1"
        var cancelButton = findChild(view, "taskCancelButton")
        verify(cancelButton !== null)
        verify(!cancelButton.enabled, "cancel disabled for non-running tasks")
    }

    function test_empty_state_when_no_tasks() {
        var view = createTemporaryObject(tasksViewComponent, testCase)
        var list = findChild(view, "tasksListView")
        compare(list.count, 0)
        var emptyLabel = findChild(view, "tasksEmptyLabel")
        verify(emptyLabel !== null, "empty label visible")
        verify(emptyLabel.visible)
    }
}
