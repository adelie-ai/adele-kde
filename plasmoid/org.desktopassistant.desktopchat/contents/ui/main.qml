import QtQuick
import QtCore
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore

PlasmoidItem {
    id: root
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    readonly property int minWidgetWidth: 460
    readonly property int minWidgetHeight: 560
    implicitWidth: 520
    implicitHeight: 620
    readonly property string xdgDataHome: String(StandardPaths.writableLocation(StandardPaths.GenericDataLocation) || "")
    readonly property string normalizedDataHome: xdgDataHome.indexOf("file://") === 0 ? xdgDataHome.substring(7) : xdgDataHome
    readonly property string sharedModuleChatViewPath: "file://" + normalizedDataHome + "/desktop-assistant/chat-module/ui/ChatView.qml"

    Plasmoid.status: (chatViewLoader.item && chatViewLoader.item.hideWidget)
        ? PlasmaCore.Types.HiddenStatus
        : PlasmaCore.Types.ActiveStatus

    Loader {
        id: chatViewLoader
        anchors.fill: parent
        property int sourceIndex: 0
        readonly property var sourceCandidates: [
            sharedModuleChatViewPath,
            Qt.resolvedUrl("./ChatView.qml")
        ]
        source: sourceCandidates[sourceIndex]
        onLoaded: {
            if (item) {
                item.panelMode = false
                if (typeof item.tasksBadgeClicked !== "undefined") {
                    item.tasksBadgeClicked.connect(function() {
                        tasksWindowLoader.show()
                    })
                }
            }
        }
        onStatusChanged: {
            if (status === Loader.Error && sourceIndex < sourceCandidates.length - 1) {
                sourceIndex += 1
                source = sourceCandidates[sourceIndex]
            }
        }
    }

    // Separate process-manager window (adele-kde#7). Loaded lazily from the
    // shared module path (preferred) with a local fallback so the
    // plasmoid still opens the window when the shared module isn't
    // synced yet. Hidden by default; the tasks badge in the chat header
    // toggles it visible.
    Loader {
        id: tasksWindowLoader
        active: true
        property int sourceIndex: 0
        readonly property var sourceCandidates: [
            "file://" + normalizedDataHome + "/desktop-assistant/chat-module/ui/TasksWindow.qml",
            Qt.resolvedUrl("./TasksWindow.qml")
        ]
        source: sourceCandidates[sourceIndex]
        function show() {
            if (item) {
                if (chatViewLoader.item && chatViewLoader.item.tasksBackend) {
                    item.backend = chatViewLoader.item.tasksBackend
                }
                item.visible = true
                if (typeof item.raise === "function") item.raise()
                if (typeof item.requestActivate === "function") item.requestActivate()
            }
        }
        onStatusChanged: {
            if (status === Loader.Error && sourceIndex < sourceCandidates.length - 1) {
                sourceIndex += 1
                source = sourceCandidates[sourceIndex]
            }
        }
    }

    Component.onCompleted: {
        if (width > 0 && width < minWidgetWidth) {
            width = minWidgetWidth
        }
        if (height > 0 && height < minWidgetHeight) {
            height = minWidgetHeight
        }
    }

    onWidthChanged: {
        if (width > 0 && width < minWidgetWidth) {
            width = minWidgetWidth
        }
    }

    onHeightChanged: {
        if (height > 0 && height < minWidgetHeight) {
            height = minWidgetHeight
        }
    }
}
