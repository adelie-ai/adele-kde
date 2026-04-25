/*
 * Multi-connection Connections page (issue adele-kde#1).
 *
 * Surfaces the daemon's `ListConnections` result as a Kirigami list. Add /
 * Configure / Remove wire to the corresponding multi-connection commands
 * exposed over the WebSocket API:
 *   ListConnections, CreateConnection, UpdateConnection, DeleteConnection.
 *
 * Per-connector editing lives in ConnectionEditor.qml (loaded inline as a
 * drawer) so the connector-specific field sets (AWS profile + region for
 * Bedrock vs. API-key-env for OpenAI/Anthropic) stay isolated.
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root
    spacing: 10

    property string statusText: ""
    property var connections: []
    property bool loading: false

    function reload() {
        loading = true
        kcm.wsCall("list_connections", {}, function(result, error) {
            loading = false
            if (error) {
                statusText = error
                return
            }
            const rows = (result && result.connections) ? result.connections : []
            const normalised = []
            for (let i = 0; i < rows.length; i++) {
                const item = rows[i] || {}
                const availability = item.availability || {}
                normalised.push({
                    id: String(item.id || ""),
                    connector_type: String(item.connector_type || ""),
                    display_label: String(item.display_label || item.id || ""),
                    availability_status: String(availability.status || "ok"),
                    availability_reason: String(availability.reason || ""),
                    has_credentials: Boolean(item.has_credentials),
                })
            }
            connections = normalised
            statusText = normalised.length === 0
                ? "No connections yet. Use Add to create one."
                : ("Loaded " + normalised.length + " connection(s)")
        })
    }

    Component.onCompleted: reload()

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: statusText.length > 0
        text: statusText
        type: Kirigami.MessageType.Information
    }

    RowLayout {
        Layout.fillWidth: true

        QQC2.Label {
            text: loading ? "Loading connections…" : ("Connections: " + connections.length)
            Layout.fillWidth: true
        }

        QQC2.Button {
            text: "Add…"
            icon.name: "list-add"
            onClicked: {
                newConnectionPicker.open()
            }
        }

        QQC2.Button {
            text: "Refresh"
            icon.name: "view-refresh"
            enabled: !loading
            onClicked: reload()
        }
    }

    ListView {
        id: listView
        Layout.fillWidth: true
        Layout.fillHeight: true
        // Without an explicit minimum, the parent ColumnLayout (wrapped in
        // an outer QQC2.ScrollView in main.qml) is height-unbounded and
        // Layout.fillHeight collapses to zero, hiding the list entirely.
        Layout.minimumHeight: 240
        implicitHeight: Math.max(contentHeight + 8, Layout.minimumHeight)
        clip: true
        model: connections
        spacing: 2
        delegate: QQC2.ItemDelegate {
            width: listView.width
            onClicked: editor.openFor(modelData)

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    Kirigami.Icon {
                        source: {
                            if (modelData.availability_status === "unavailable") return "emblem-warning"
                            if (!modelData.has_credentials) return "emblem-warning"
                            return "emblem-ok"
                        }
                        implicitWidth: Kirigami.Units.iconSizes.medium
                        implicitHeight: Kirigami.Units.iconSizes.medium
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 0

                        QQC2.Label {
                            text: modelData.display_label
                            font.bold: true
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        QQC2.Label {
                            text: modelData.connector_type + (modelData.has_credentials ? " · credentials present" : " · no credentials")
                            color: Kirigami.Theme.disabledTextColor
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        QQC2.Label {
                            visible: modelData.availability_status !== "ok"
                            text: "Unavailable: " + (modelData.availability_reason || "unknown")
                            color: Kirigami.Theme.negativeTextColor
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    QQC2.Button {
                        text: "Configure"
                        icon.name: "configure"
                        onClicked: editor.openFor(modelData)
                    }

                    QQC2.Button {
                        text: "Remove"
                        icon.name: "edit-delete"
                        onClicked: removeDialog.promptFor(modelData)
                    }
                }
        }
    }

    // --- Add-connection chooser ------------------------------------------------

    QQC2.Dialog {
        id: newConnectionPicker
        title: "Add connection"
        modal: true
        standardButtons: QQC2.Dialog.Cancel
        anchors.centerIn: parent
        implicitWidth: 360

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            QQC2.Label {
                text: "Choose connector type:"
                Layout.fillWidth: true
            }

            Repeater {
                model: [
                    { type: "anthropic", label: "Anthropic (Claude)" },
                    { type: "openai", label: "OpenAI-compatible" },
                    { type: "bedrock", label: "AWS Bedrock" },
                    { type: "ollama", label: "Ollama" },
                ]
                delegate: QQC2.Button {
                    Layout.fillWidth: true
                    text: modelData.label
                    onClicked: {
                        newConnectionPicker.close()
                        editor.openForNew(modelData.type)
                    }
                }
            }
        }
    }

    // --- Remove-confirmation dialog -------------------------------------------

    QQC2.Dialog {
        id: removeDialog
        modal: true
        anchors.centerIn: parent
        title: "Remove connection"
        implicitWidth: 420

        property var target: null
        property string errorText: ""

        function promptFor(item) {
            target = item
            errorText = ""
            open()
        }

        standardButtons: QQC2.Dialog.Cancel | QQC2.Dialog.Ok

        onAccepted: {
            if (!target) return
            const id = target.id
            kcm.wsCall("delete_connection", { id: id, force: false }, function(result, error) {
                if (error) {
                    // The daemon refuses deletion when a purpose still
                    // references the connection; offer a Force variant.
                    removeDialog.errorText = error
                    forceDialog.promptFor(target, error)
                    removeDialog.close()
                    return
                }
                statusText = "Removed connection '" + id + "'"
                reload()
            })
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 6

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: removeDialog.target
                    ? ("Permanently remove '" + removeDialog.target.id + "'? Purposes referencing it will need re-assigning.")
                    : ""
            }

            QQC2.Label {
                visible: removeDialog.errorText.length > 0
                color: Kirigami.Theme.negativeTextColor
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                text: removeDialog.errorText
            }
        }
    }

    // --- Force-remove dialog (shown when the daemon refuses a plain delete) ---

    QQC2.Dialog {
        id: forceDialog
        modal: true
        anchors.centerIn: parent
        title: "Force-remove connection"
        implicitWidth: 460
        standardButtons: QQC2.Dialog.Cancel | QQC2.Dialog.Yes

        property var target: null
        property string refusalReason: ""

        function promptFor(item, reason) {
            target = item
            refusalReason = reason || ""
            open()
        }

        onAccepted: {
            if (!target) return
            const id = target.id
            kcm.wsCall("delete_connection", { id: id, force: true }, function(result, error) {
                if (error) {
                    statusText = "Force-remove failed: " + error
                    return
                }
                statusText = "Force-removed connection '" + id + "'. Referencing purposes fell back to the interactive purpose."
                reload()
            })
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 6

            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: forceDialog.target
                    ? ("The daemon refused to remove '" + forceDialog.target.id + "':\n\n"
                       + forceDialog.refusalReason + "\n\nForce-remove will fall back referencing purposes to the interactive purpose.")
                    : ""
            }
        }
    }

    // --- Per-type configuration editor ----------------------------------------

    ConnectionEditor {
        id: editor
        onDone: function(succeeded) {
            if (succeeded) {
                statusText = "Saved."
                reload()
            }
        }
    }
}
