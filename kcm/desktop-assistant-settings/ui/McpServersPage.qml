/*
 * MCP Servers page (mcp-servers-ui epic, KCM side).
 *
 * Surfaces the daemon's `ListMcpServersJson` result as a Kirigami list, one
 * row per configured MCP server with an *honest* per-server state (the daemon
 * computes running / stopped / needs_auth / auth_expired / error / disabled,
 * not a flat "stopped"). Add / Edit / Remove wire to the transport-aware write
 * path over D-Bus:
 *   ListMcpServersJson, UpsertMcpServer, RemoveMcpServer, SetMcpServerEnabled.
 *
 * The daemon is headless so it never opens a GUI — for the subset of servers
 * that need configuration/login it only *reports* an argv (`configure_command`)
 * plus a button label (`configure_label`); this page spawns it detached via
 * kcm.launchMcpConfigure. Absent label = no button.
 *
 * Per-server create/edit lives in McpServerEditor.qml (loaded inline as an
 * overlay sheet) so the transport-specific field sets (stdio command/args/env
 * vs. remote url + bearer/OAuth) stay isolated, mirroring ConnectionsPage +
 * ConnectionEditor.
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root
    spacing: 10

    property string statusText: ""
    property var servers: []
    property bool loading: false

    function reload() {
        loading = true
        kcm.daemonCall("list_mcp_servers", {}, function(result, error) {
            loading = false
            if (error) {
                statusText = error
                return
            }
            // ListMcpServersJson returns a JSON *array* (not an object wrapper),
            // which the C++ edge JSON.parses into a JS array before it reaches
            // us. Guard the shape defensively so a daemon that ever wrapped it
            // can't throw here.
            const rows = Array.isArray(result) ? result : []
            const normalised = []
            for (let i = 0; i < rows.length; i++) {
                const item = rows[i] || {}
                normalised.push({
                    name: String(item.name || ""),
                    command: String(item.command || ""),
                    args: Array.isArray(item.args) ? item.args : [],
                    namespace: String(item.namespace || ""),
                    enabled: Boolean(item.enabled),
                    status: String(item.status || "stopped"),
                    tool_count: Number(item.tool_count || 0),
                    transport: String(item.transport || "stdio"),
                    target: String(item.target || ""),
                    detail: String(item.detail || ""),
                    configure_label: String(item.configure_label || ""),
                    configure_command: Array.isArray(item.configure_command) ? item.configure_command : [],
                    auth_kind: String(item.auth_kind || ""),
                    oauth_authorized: Boolean(item.oauth_authorized),
                    oauth_account: String(item.oauth_account || ""),
                    oauth_scopes: Array.isArray(item.oauth_scopes) ? item.oauth_scopes : [],
                })
            }
            servers = normalised
            statusText = normalised.length === 0
                ? "No MCP servers configured yet. Use Add to create one."
                : ("Loaded " + normalised.length + " server(s)")
        })
    }

    Component.onCompleted: reload()

    // Map the daemon's honest state enum onto an icon + a human label. The
    // daemon owns the distinctions (binary missing, unreachable, never
    // authorized, auth expired) so this page never re-derives them.
    function stateIcon(status) {
        if (status === "running") return "emblem-ok"
        if (status === "needs_auth" || status === "auth_expired") return "emblem-warning"
        if (status === "error") return "emblem-error"
        // stopped / disabled → neutral
        return "emblem-unmounted"
    }

    function stateLabel(status) {
        if (status === "running") return "Running"
        if (status === "stopped") return "Stopped"
        if (status === "disabled") return "Disabled"
        if (status === "needs_auth") return "Sign in required"
        if (status === "auth_expired") return "Sign in expired"
        if (status === "error") return "Error"
        return status
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: statusText.length > 0
        text: statusText
        type: Kirigami.MessageType.Information
    }

    RowLayout {
        Layout.fillWidth: true

        QQC2.Label {
            text: loading ? "Loading MCP servers…" : ("MCP servers: " + servers.length)
            Layout.fillWidth: true
        }

        QQC2.Button {
            text: "Add…"
            icon.name: "list-add"
            onClicked: editor.openForNew()
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
        // Without an explicit minimum, the parent ColumnLayout (wrapped in an
        // outer QQC2.ScrollView in main.qml) is height-unbounded and
        // Layout.fillHeight collapses to zero, hiding the list entirely.
        Layout.minimumHeight: 240
        implicitHeight: Math.max(contentHeight + 8, Layout.minimumHeight)
        clip: true
        model: servers
        spacing: 2
        delegate: QQC2.ItemDelegate {
            width: listView.width
            onClicked: editor.openFor(modelData)

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    Kirigami.Icon {
                        source: root.stateIcon(modelData.status)
                        implicitWidth: Kirigami.Units.iconSizes.medium
                        implicitHeight: Kirigami.Units.iconSizes.medium
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            QQC2.Label {
                                text: modelData.name
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            // Transport chip: local (stdio) vs remote (HTTP).
                            Rectangle {
                                radius: Kirigami.Units.smallSpacing
                                color: "transparent"
                                border.color: Kirigami.Theme.disabledTextColor
                                border.width: 1
                                implicitWidth: transportChipLabel.implicitWidth + Kirigami.Units.smallSpacing * 2
                                implicitHeight: transportChipLabel.implicitHeight + Kirigami.Units.smallSpacing
                                QQC2.Label {
                                    id: transportChipLabel
                                    anchors.centerIn: parent
                                    text: modelData.transport === "http" ? "remote" : "local"
                                    font: Kirigami.Theme.smallFont
                                    color: Kirigami.Theme.disabledTextColor
                                }
                            }
                        }

                        QQC2.Label {
                            text: root.stateLabel(modelData.status)
                                + (modelData.status === "running" && modelData.tool_count > 0
                                    ? (" · " + modelData.tool_count + " tool(s)") : "")
                                + (modelData.target.length > 0 ? (" · " + modelData.target) : "")
                            color: Kirigami.Theme.disabledTextColor
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        // Last connect error (only populated by the daemon when
                        // the server failed to connect).
                        QQC2.Label {
                            visible: modelData.detail.length > 0
                            text: modelData.detail
                            color: Kirigami.Theme.negativeTextColor
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    // Enable/disable toggle. Toggling reloads so the daemon's
                    // recomputed state (a disabled server reports "disabled")
                    // is reflected.
                    QQC2.Switch {
                        checked: modelData.enabled
                        onToggled: {
                            kcm.daemonCall("set_mcp_server_enabled",
                                { name: modelData.name, enabled: checked },
                                function(result, error) {
                                    if (error) {
                                        statusText = error
                                    }
                                    reload()
                                })
                        }
                    }

                    // Configure / Sign in — shown only when the daemon reports a
                    // label (OAuth servers in Phase 1). Spawns the daemon's argv
                    // detached; the browser flow writes the refresh token, then a
                    // reload flips the state toward running.
                    QQC2.Button {
                        visible: modelData.configure_label.length > 0
                        text: modelData.configure_label
                        icon.name: "config-users"
                        onClicked: {
                            kcm.launchMcpConfigure(modelData.configure_command)
                            statusText = "Launched: " + modelData.configure_label
                                + " (finish in the browser, then Refresh)"
                        }
                    }

                    QQC2.Button {
                        text: "Edit"
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

    // --- Remove-confirmation dialog -------------------------------------------

    QQC2.Dialog {
        id: removeDialog
        modal: true
        anchors.centerIn: parent
        title: "Remove MCP server"
        implicitWidth: 420

        property var target: null

        function promptFor(item) {
            target = item
            open()
        }

        standardButtons: QQC2.Dialog.Cancel | QQC2.Dialog.Ok

        onAccepted: {
            if (!target) return
            const name = target.name
            kcm.daemonCall("remove_mcp_server", { name: name }, function(result, error) {
                if (error) {
                    statusText = "Remove failed: " + error
                    return
                }
                statusText = "Removed MCP server '" + name + "'"
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
                    ? ("Permanently remove the MCP server '" + removeDialog.target.name
                       + "'? Its tools will no longer be available to the assistant.")
                    : ""
            }
        }
    }

    // --- Transport-aware create/edit editor -----------------------------------

    McpServerEditor {
        id: editor
        onDone: function(succeeded) {
            if (succeeded) {
                statusText = "Saved."
                reload()
            }
        }
    }
}
