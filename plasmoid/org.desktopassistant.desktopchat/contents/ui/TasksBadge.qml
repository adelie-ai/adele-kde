// Tasks badge for the chat header (adele-kde#7).
//
// Visible only when ``backend.runningTaskCount > 0``. The label reads
// "(N running)" so the user can tell at a glance how many standalone
// agents / subagents are still in flight. Clicking the badge emits
// ``clicked()``; the parent (main.qml) wires that to opening the
// separate process-manager window.
//
// The backend is dependency-injected (rather than imported as a
// singleton) so the QML autotests can swap in a stub QtObject with the
// same ``runningTaskCount`` property.

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

QQC2.AbstractButton {
    id: root

    // The backend just needs to expose a `runningTaskCount` property.
    // Anything that satisfies that contract is fair game — the real
    // backend is the live tasks model in ChatView; the tests use a
    // stub QtObject.
    property var backend: null

    readonly property int runningTaskCount: backend ? Number(backend.runningTaskCount || 0) : 0
    readonly property bool hasRunningTasks: runningTaskCount > 0

    visible: hasRunningTasks
    enabled: hasRunningTasks

    text: hasRunningTasks ? ("(" + runningTaskCount + " running)") : ""

    // Programmatic activation used by autotests; mirrors the click path
    // so signal handlers fire the same way.
    function activate() {
        clicked()
    }

    padding: 4
    hoverEnabled: true

    background: Rectangle {
        radius: 6
        color: root.hovered ? Kirigami.Theme.highlightColor : Kirigami.Theme.alternateBackgroundColor
        border.width: 1
        border.color: Kirigami.Theme.disabledTextColor
        opacity: root.hovered ? 0.9 : 0.6
    }

    contentItem: QQC2.Label {
        text: root.text
        color: root.hovered ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
