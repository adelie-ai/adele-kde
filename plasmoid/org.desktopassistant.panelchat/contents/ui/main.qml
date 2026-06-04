import QtQuick
import QtQuick.Layouts
import QtCore
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PlasmaComponents

PlasmoidItem {
    id: root
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    readonly property string xdgDataHome: String(StandardPaths.writableLocation(StandardPaths.GenericDataLocation) || "")
    readonly property string normalizedDataHome: xdgDataHome.indexOf("file://") === 0 ? xdgDataHome.substring(7) : xdgDataHome
    readonly property string sharedModuleChatViewPath: "file://" + normalizedDataHome + "/desktop-assistant/chat-module/ui/ChatView.qml"

    preferredRepresentation: compactRepresentation
    switchWidth: 460
    switchHeight: 560
    Plasmoid.status: PlasmaCore.Types.ActiveStatus

    // The chat view lives inside the (lazily-instantiated) fullRepresentation,
    // so the top-level contextual action below can't reach it directly. The
    // loader publishes its item here on load and clears it on teardown.
    property Item chatView: null

    // "Enable 'Hey Adele'" lives in the plasmoid's right-click menu
    // (adele-kde#29). Visible only once the popup has been opened and the voice
    // daemon is on the bus, so it never dangles as a dead toggle. The chat view
    // owns the D-Bus plumbing; this action just reflects/forwards it.
    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: "Enable “Hey Adele”"
            icon.name: "audio-input-microphone"
            checkable: true
            visible: root.chatView ? root.chatView.voiceAvailable : false
            checked: root.chatView ? root.chatView.voiceEnabled : false
            onTriggered: {
                if (root.chatView) {
                    root.chatView.setVoiceEnabled(checked)
                }
            }
        }
    ]

    compactRepresentation: PlasmaComponents.ToolButton {
        text: "Adele AI"
        icon.source: Qt.resolvedUrl("../images/adele.png")
        icon.width: Kirigami.Units.iconSizes.smallMedium
        icon.height: Kirigami.Units.iconSizes.smallMedium
        onClicked: root.expanded = !root.expanded
    }

    fullRepresentation: Item {
        Layout.minimumWidth: 460
        Layout.minimumHeight: 560

        Component.onCompleted: {
            if (width > 0 && width < 460) {
                width = 460
            }
            if (height > 0 && height < 560) {
                height = 560
            }
        }

        Loader {
            id: chatViewLoader
            anchors.fill: parent
            property int sourceIndex: 0
            readonly property var sourceCandidates: [
                sharedModuleChatViewPath,
                Qt.resolvedUrl("../../../org.desktopassistant.desktopchat/contents/ui/ChatView.qml")
            ]
            source: sourceCandidates[sourceIndex]
            onLoaded: {
                if (item) {
                    item.panelMode = true
                    root.chatView = item
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
            Component.onDestruction: {
                if (root.chatView === item) {
                    root.chatView = null
                }
            }
        }
    }

    // Separate process-manager window (adele-kde#7). The popup is
    // height-constrained, so the tasks list lives in its own window.
    // Loaded lazily from the shared module path with a fallback to the
    // desktopchat copy.
    Loader {
        id: tasksWindowLoader
        active: true
        property int sourceIndex: 0
        readonly property var sourceCandidates: [
            "file://" + normalizedDataHome + "/desktop-assistant/chat-module/ui/TasksWindow.qml",
            Qt.resolvedUrl("../../../org.desktopassistant.desktopchat/contents/ui/TasksWindow.qml")
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
}
