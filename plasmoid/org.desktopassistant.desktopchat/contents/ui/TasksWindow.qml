// Separate KDE window that hosts the process-manager view.
//
// Declared by the plasmoid root (main.qml) with `visible: false`. The
// tasks badge in the chat header toggles it visible.

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Window
import org.kde.kirigami as Kirigami

Window {
    id: tasksWindowRoot

    property var backend: null

    title: "Adele Tasks"
    width: 720
    height: 480
    minimumWidth: 480
    minimumHeight: 320
    flags: Qt.Window | Qt.WindowCloseButtonHint | Qt.WindowMinMaxButtonsHint

    color: Kirigami.Theme.backgroundColor

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TasksView {
            id: tasksView
            Layout.fillWidth: true
            Layout.fillHeight: true
            backend: tasksWindowRoot.backend
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            spacing: 6

            Item { Layout.fillWidth: true }

            QQC2.Button {
                id: tasksWindowCloseButton
                objectName: "tasksWindowCloseButton"
                text: "Close"
                onClicked: tasksWindowRoot.visible = false
            }
        }
    }

    onClosing: function(close) {
        // Closing via the window manager's X button hides rather than
        // destroys the window so the plasmoid can re-show it without
        // re-instantiating.
        tasksWindowRoot.visible = false
        close.accepted = false
    }
}
