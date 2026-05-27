// TasksView: the process-manager body shown inside TasksWindow.qml.
//
// Layout: list of tasks on the left, log view + action row on the right.
// All daemon I/O is funneled through ``backend`` so this component is
// testable in isolation with a stub QtObject. The backend contract:
//
//   tasks: array of TaskView dicts (id, title, status, ended_at,
//          progress_hint, last_error, kind, ...)
//   cancelTask(id)
//   openConversation(id)
//   refreshLogs(id)   -- request a fresh page of logs for `id`
//   taskLogs(id)      -- return the current log buffer string
//
// The component does not subscribe to changes inside ``backend``; the
// parent rebinds ``backend.tasks`` whenever the underlying list shifts.

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    id: root

    property var backend: null
    property string selectedTaskId: ""

    readonly property var tasksList: backend ? (backend.tasks || []) : []

    function selectTaskAt(index) {
        if (index < 0 || index >= tasksList.length) {
            selectedTaskId = ""
            return
        }
        const entry = tasksList[index]
        if (entry && entry.id) {
            selectedTaskId = String(entry.id)
        }
    }

    function indexOfSelected() {
        for (let i = 0; i < tasksList.length; i++) {
            if (tasksList[i] && tasksList[i].id === selectedTaskId) {
                return i
            }
        }
        return -1
    }

    function selectedTask() {
        const idx = indexOfSelected()
        return idx >= 0 ? tasksList[idx] : null
    }

    function isRunningStatus(statusValue) {
        const s = String(statusValue || "").toLowerCase()
        return s === "running" || s === "pending"
    }

    onSelectedTaskIdChanged: {
        if (selectedTaskId.length > 0 && backend && typeof backend.refreshLogs === "function") {
            backend.refreshLogs(selectedTaskId)
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // ---- Left column: list of tasks --------------------------------
        ColumnLayout {
            Layout.fillWidth: false
            Layout.preferredWidth: Math.max(220, parent.width * 0.35)
            Layout.fillHeight: true
            spacing: 4

            QQC2.Label {
                text: "Tasks"
                font.bold: true
            }

            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ListView {
                    id: tasksListView
                    objectName: "tasksListView"
                    model: root.tasksList
                    spacing: 2
                    currentIndex: root.indexOfSelected()
                    onCurrentIndexChanged: root.selectTaskAt(currentIndex)

                    delegate: QQC2.ItemDelegate {
                        id: taskDelegate
                        required property var modelData
                        required property int index
                        width: ListView.view ? ListView.view.width : implicitWidth
                        highlighted: ListView.isCurrentItem

                        contentItem: ColumnLayout {
                            spacing: 1
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: String(taskDelegate.modelData.title || taskDelegate.modelData.id || "?")
                                elide: Text.ElideRight
                                font.bold: root.isRunningStatus(taskDelegate.modelData.status)
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: {
                                    const status = String(taskDelegate.modelData.status || "")
                                    const hint = String(taskDelegate.modelData.progress_hint || "")
                                    return hint.length > 0 ? (status + " — " + hint) : status
                                }
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: Kirigami.Theme.smallFont.pointSize
                                elide: Text.ElideRight
                            }
                        }

                        onClicked: {
                            tasksListView.currentIndex = index
                            root.selectTaskAt(index)
                        }
                    }
                }
            }

            QQC2.Label {
                id: tasksEmptyLabel
                objectName: "tasksEmptyLabel"
                Layout.fillWidth: true
                visible: tasksListView.count === 0
                text: "No background tasks."
                color: Kirigami.Theme.disabledTextColor
                horizontalAlignment: Text.AlignHCenter
            }
        }

        // ---- Right column: details -------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 4

            QQC2.Label {
                id: detailTitleLabel
                objectName: "taskDetailTitleLabel"
                Layout.fillWidth: true
                font.bold: true
                text: {
                    const task = root.selectedTask()
                    return task ? String(task.title || task.id || "") : "Select a task"
                }
                elide: Text.ElideRight
            }

            QQC2.Label {
                Layout.fillWidth: true
                color: Kirigami.Theme.disabledTextColor
                text: {
                    const task = root.selectedTask()
                    if (!task) return ""
                    const error = String(task.last_error || "")
                    const status = String(task.status || "")
                    return error.length > 0 ? (status + ": " + error) : status
                }
                wrapMode: Text.WordWrap
            }

            // Use a raw TextEdit (not QQC2.TextArea) for the log buffer.
            // The Breeze style override for TextArea trips a
            // "Unable to assign TextArea to QQuickTextInput" QML warning
            // under qmltestrunner; warnings are failures, so we take
            // the simpler TextEdit and keep the surface clean.
            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                TextEdit {
                    id: taskLogsArea
                    objectName: "taskLogsArea"
                    readOnly: true
                    selectByMouse: true
                    selectByKeyboard: true
                    wrapMode: TextEdit.Wrap
                    color: Kirigami.Theme.textColor
                    text: {
                        const task = root.selectedTask()
                        if (!task || !root.backend || typeof root.backend.taskLogs !== "function") {
                            return ""
                        }
                        return String(root.backend.taskLogs(task.id) || "")
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                QQC2.Button {
                    id: taskCancelButton
                    objectName: "taskCancelButton"
                    text: "Cancel"
                    enabled: {
                        const task = root.selectedTask()
                        return task !== null && root.isRunningStatus(task.status)
                    }
                    onClicked: {
                        const task = root.selectedTask()
                        if (task && root.backend && typeof root.backend.cancelTask === "function") {
                            root.backend.cancelTask(task.id)
                        }
                    }
                }

                QQC2.Button {
                    id: taskOpenConversationButton
                    objectName: "taskOpenConversationButton"
                    text: "Open Conversation"
                    enabled: root.selectedTask() !== null
                    onClicked: {
                        const task = root.selectedTask()
                        if (task && root.backend && typeof root.backend.openConversation === "function") {
                            root.backend.openConversation(task.id)
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                QQC2.Button {
                    objectName: "taskRefreshLogsButton"
                    text: "Refresh Logs"
                    enabled: root.selectedTask() !== null
                    onClicked: {
                        const task = root.selectedTask()
                        if (task && root.backend && typeof root.backend.refreshLogs === "function") {
                            root.backend.refreshLogs(task.id)
                        }
                    }
                }
            }
        }
    }
}
