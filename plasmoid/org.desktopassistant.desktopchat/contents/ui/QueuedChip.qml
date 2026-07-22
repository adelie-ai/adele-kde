// QueuedChip.qml — one chip in the message-queue row of the chat composer.
//
// Extracted verbatim from ChatView's Repeater delegate so the click->index
// wiring is unit-testable in qmltestrunner: ChatView itself pulls in the
// Plasmoid and native-client (org.desktopassistant.client) modules and can't be
// instantiated in a generic test env, but this chip depends only on QtQuick +
// Kirigami (available headless), so a test can click it and assert what it
// emits.
//
// The chip renders a one-line preview of a queued prompt with an edit
// affordance (click the label) and a remove affordance (the ✕ button). Clicking
// the label translates the VISIBLE chip index to the full-queue index the
// reducer's EditQueued expects (via QueueRecall.chipEditIndex) before reporting
// it — a checked-out item is absent from the visible list but reinserted at its
// original slot before indexing. Both affordances report through signals so the
// parent (ChatView) owns the `core` wiring.

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

import "QueueRecall.js" as QueueRecall

Rectangle {
    id: chip

    // Position within the VISIBLE (event) queue, the chip's preview text, and
    // the full-queue index currently checked out for editing (-1 = none) —
    // injected by ChatView's Repeater delegate.
    property int chipIndex: 0
    property string chipText: ""
    property int editingIndex: -1

    // Styling injected from ChatView so the chip matches the composer exactly.
    property real uiScale: 1.0
    property real baseFontPointSize: Math.max(1, Number(Kirigami.Theme.defaultFont.pointSize || Qt.application.font.pointSize || 10))

    // Click the label to recall this chip for editing (reported as its
    // full-queue index); the ✕ removes it (reported as its visible index, which
    // is what RemoveQueued expects). The parent wires these to the core.
    signal editRequested(int fullIndex)
    signal removeRequested(int index)

    implicitHeight: Math.round(26 * uiScale)
    implicitWidth: queuedChipRow.implicitWidth + Math.round(12 * uiScale)
    radius: implicitHeight / 2
    color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.12)
    border.width: 1
    border.color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.4)

    RowLayout {
        id: queuedChipRow
        anchors.centerIn: parent
        spacing: Math.round(2 * chip.uiScale)

        QQC2.Label {
            Layout.leftMargin: Math.round(6 * chip.uiScale)
            Layout.alignment: Qt.AlignVCenter
            Layout.maximumWidth: Math.round(200 * chip.uiScale)
            text: QueueRecall.previewText(chip.chipText)
            color: Kirigami.Theme.textColor
            elide: Text.ElideRight
            font.pointSize: chip.baseFontPointSize * 0.9

            MouseArea {
                objectName: "queuedChipEditArea"
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                // Translate the visible index to the full-queue index EditQueued
                // expects (a checked-out item is absent here but reinserted
                // before indexing).
                onClicked: chip.editRequested(
                    QueueRecall.chipEditIndex(chip.chipIndex, chip.editingIndex))
            }
        }

        QQC2.ToolButton {
            objectName: "queuedChipRemoveButton"
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: Math.round(22 * chip.uiScale)
            implicitHeight: Math.round(22 * chip.uiScale)
            icon.name: "dialog-close"
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: "Remove from queue"
            QQC2.ToolTip.visible: hovered
            onClicked: chip.removeRequested(chip.chipIndex)
        }
    }
}
