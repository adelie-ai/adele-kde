import QtQuick
import QtQuick.Layouts
import QtCore
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PlasmaComponents
import org.desktopassistant.client

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

    // --- Panel voice state (so the taskbar icon overlay works while collapsed)
    // The full ChatView owns a VoiceController, but it only EXISTS while the
    // popup is expanded. For the compact (taskbar) icon to show a "mic is
    // listening" badge while collapsed, a root-level controller lives here. It is
    // signal-driven (subscribes to the voice daemon's StateChanged and watches
    // the service name's ownership), so the badge reflects Listening/Processing/
    // Speaking live with no polling and no subprocess, and degrades cleanly
    // (available == false ⇒ controls hidden) whenever the daemon isn't on the bus.
    VoiceController {
        id: rootVoice
        Component.onCompleted: start()
    }

    // Effective state the badge renders from: prefer the live chat view when it
    // is loaded (its controller and ours track the same daemon, so they agree),
    // else the root controller.
    readonly property bool voiceAvailable: chatView ? chatView.voiceAvailable : rootVoice.available
    readonly property string voiceState: chatView ? chatView.voiceState : rootVoice.state
    readonly property bool voiceListening: voiceAvailable && voiceState === "Listening"
    readonly property bool voiceSpeaking: voiceAvailable && voiceState === "Speaking"
    // The daemon is thinking (STT/LLM working). Its own badge state so the
    // taskbar icon never looks idle while a turn is actually in flight
    // (adele-kde#38).
    readonly property bool voiceProcessing: voiceAvailable && voiceState === "Processing"
    // Any in-flight turn (mic open, thinking, or talking back).
    readonly property bool voiceActive: voiceAvailable && voiceState !== "Idle"

    // Badge accent per state, mirroring the chat view's voiceStateColor so the
    // collapsed panel dot and the expanded in-widget chip always agree:
    //   Listening → negative/red, Processing → neutral/amber, Speaking → blue.
    readonly property color voiceBadgeColor: {
        if (voiceListening) return Kirigami.Theme.negativeTextColor
        if (voiceProcessing) return Kirigami.Theme.neutralTextColor
        return Kirigami.Theme.highlightColor
    }

    // Abort the current turn back to Idle/wake-listening. When the popup is open
    // the chat view owns the voice plumbing, so defer to it; while collapsed
    // (chatView is null) drive the root controller directly so the panel's cancel
    // action works without expanding the widget. State clears itself when the
    // daemon's StateChanged lands — no optimistic flip needed.
    function cancelVoiceTurn() {
        if (!voiceActive) {
            return
        }
        if (chatView) {
            chatView.voiceCancelTurn()
            return
        }
        if (voiceState === "Speaking") {
            rootVoice.stopSpeaking()
        } else {
            rootVoice.stopListening()
        }
    }

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
        },
        // Abort the active turn straight from the panel's right-click menu
        // (adele-kde#38) without expanding the widget. Shown only while a turn
        // is in flight so it never dangles as a dead control.
        PlasmaCore.Action {
            text: "Stop — cancel current turn"
            icon.name: "dialog-cancel"
            visible: root.voiceActive
            onTriggered: root.cancelVoiceTurn()
        }
    ]

    compactRepresentation: PlasmaComponents.ToolButton {
        id: compactRoot
        text: "Adele AI"
        // Swap to the "thinking" Adele avatar while the daemon is Processing so
        // the panel icon itself reflects the state (adele-kde#38), not just the
        // corner badge. Idle/Listening/Speaking keep the default avatar.
        icon.source: root.voiceProcessing
            ? Qt.resolvedUrl("../images/adele_thinking.png")
            : Qt.resolvedUrl("../images/adele.png")
        icon.width: Kirigami.Units.iconSizes.smallMedium
        icon.height: Kirigami.Units.iconSizes.smallMedium
        onClicked: root.expanded = !root.expanded

        // Voice-state badge overlaid on the taskbar icon (adele-kde voice-state).
        // A small coloured dot in the corner makes the pipeline state obvious
        // even while the chat popup is collapsed:
        //   Listening → red + brisk breathing (recording — the one to notice),
        //   Processing→ amber + slow breathing (thinking — never looks idle),
        //   Speaking  → blue, solid (the assistant is talking back).
        // Hidden at Idle / when the voice service isn't running so the icon
        // stays clean.
        Rectangle {
            id: voiceBadge
            visible: root.voiceListening || root.voiceProcessing || root.voiceSpeaking
            // Anchor to the icon's bottom-right corner. The ToolButton centres
            // its icon, so derive the corner from the icon box rather than the
            // (often wider) button so the badge hugs the glyph.
            readonly property real iconBox: Math.min(compactRoot.width, compactRoot.height)
            width: Math.max(7, Math.round(iconBox * 0.34))
            height: width
            radius: width / 2
            color: root.voiceBadgeColor
            // A thin contrasting outline so the dot reads on any icon/panel hue.
            border.width: Math.max(1, Math.round(width * 0.16))
            border.color: Kirigami.Theme.backgroundColor
            x: compactRoot.width / 2 + iconBox / 2 - width
            y: compactRoot.height / 2 + iconBox / 2 - height

            // Breathing pulse while recording so an open mic is unmissable in
            // the panel. Solid (no pulse) for the calmer Speaking state.
            SequentialAnimation on opacity {
                running: voiceBadge.visible && root.voiceListening
                loops: Animation.Infinite
                alwaysRunToEnd: true
                NumberAnimation { from: 1.0; to: 0.35; duration: 750; easing.type: Easing.InOutSine }
                NumberAnimation { from: 0.35; to: 1.0; duration: 750; easing.type: Easing.InOutSine }
            }
            // Slower pulse while thinking so a busy turn reads as alive but
            // calmer than an open mic.
            SequentialAnimation on opacity {
                running: voiceBadge.visible && root.voiceProcessing
                loops: Animation.Infinite
                alwaysRunToEnd: true
                NumberAnimation { from: 1.0; to: 0.45; duration: 1100; easing.type: Easing.InOutSine }
                NumberAnimation { from: 0.45; to: 1.0; duration: 1100; easing.type: Easing.InOutSine }
            }
        }
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
