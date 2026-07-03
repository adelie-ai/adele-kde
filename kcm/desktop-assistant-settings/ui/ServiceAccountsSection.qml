/*
 * Service Accounts section for the Authentication tab (typed-OAuth /
 * service-accounts epic — adelie-ai/desktop-assistant#477 / adele-kde#105).
 *
 * The *outbound* half of the Auth tab: reusable OAuth credentials Adele uses to
 * reach external services (Gmail/Calendar/Drive…). Distinct from the inbound
 * WebSocket/OIDC config above it, which governs who may connect *to* Adele. Each
 * kind is labelled with its purpose so the two are never confused.
 *
 * Daemon-backed via the JSON-at-the-edge D-Bus methods ListServiceAccountsJson /
 * UpsertServiceAccount / RemoveServiceAccount. Sign-in reuses the daemon-reported
 * argv (configure_command) spawned detached via kcm.launchMcpConfigure — the
 * daemon owns its binary path, exactly like the MCP-server Sign-in action.
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root
    spacing: 8

    property var accounts: []
    property bool loading: false
    property string statusText: ""

    function reload() {
        loading = true
        kcm.daemonCall("list_service_accounts", {}, function(result, error) {
            loading = false
            if (error) {
                statusText = error
                return
            }
            // ListServiceAccountsJson returns a *top-level* JSON array. The C++
            // edge yields an array-*like* value (has .length) that is not a
            // strict JS Array, so gate on .length, not Array.isArray (same gotcha
            // as McpServersPage). Tolerate a {accounts:[...]} wrapper or a raw
            // JSON string too.
            let rows = []
            if (typeof result === "string") {
                try { rows = JSON.parse(result) } catch (e) { rows = [] }
            } else if (result && result.accounts !== undefined) {
                rows = result.accounts
            } else if (result && result.length !== undefined) {
                rows = result
            }
            if (!rows || rows.length === undefined) { rows = [] }
            const normalised = []
            for (let i = 0; i < rows.length; i++) {
                const item = rows[i] || {}
                normalised.push({
                    id: String(item.id || ""),
                    display_name: String(item.display_name || ""),
                    client_id: String(item.client_id || ""),
                    client_secret_ref: String(item.client_secret_ref || ""),
                    authorize_url: String(item.authorize_url || ""),
                    token_url: String(item.token_url || ""),
                    account: String(item.account || ""),
                    refresh_token_ref: String(item.refresh_token_ref || ""),
                    granted_scopes: Array.isArray(item.granted_scopes) ? item.granted_scopes : [],
                    authorized: Boolean(item.authorized),
                    configure_label: String(item.configure_label || ""),
                    configure_command: Array.isArray(item.configure_command) ? item.configure_command : [],
                })
            }
            accounts = normalised
            statusText = ""
        })
    }

    Component.onCompleted: reload()

    RowLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing
        QQC2.Label {
            text: "Service accounts"
            font.bold: true
        }
        InfoTip {
            text: "Credentials Adele uses to reach external services (outbound). "
                + "Enter an OAuth client once here; MCP servers reference an "
                + "account instead of repeating it, and one sign-in serves every "
                + "server that shares it."
        }
        Item { Layout.fillWidth: true }
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

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font: Kirigami.Theme.smallFont
        color: Kirigami.Theme.disabledTextColor
        text: "Credentials Adele uses to reach external services (outbound)."
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: statusText.length > 0
        text: statusText
        type: Kirigami.MessageType.Warning
    }

    QQC2.Label {
        Layout.fillWidth: true
        visible: accounts.length === 0 && !loading
        wrapMode: Text.Wrap
        color: Kirigami.Theme.disabledTextColor
        text: "No service accounts yet. Use Add to create one (e.g. a Google "
            + "Workspace OAuth client for Gmail/Calendar/Drive)."
    }

    Repeater {
        model: accounts
        delegate: Kirigami.AbstractCard {
            Layout.fillWidth: true
            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Icon {
                    source: modelData.authorized ? "emblem-ok" : "emblem-warning"
                    implicitWidth: Kirigami.Units.iconSizes.medium
                    implicitHeight: Kirigami.Units.iconSizes.medium
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 0
                    QQC2.Label {
                        text: modelData.display_name.length > 0
                            ? modelData.display_name : modelData.id
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    QQC2.Label {
                        text: (modelData.authorized ? "Signed in" : "Sign in required")
                            + (modelData.account.length > 0 ? (" · " + modelData.account) : "")
                            + (modelData.granted_scopes.length > 0
                                ? (" · " + modelData.granted_scopes.length + " scope(s)") : "")
                        color: Kirigami.Theme.disabledTextColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                QQC2.Button {
                    visible: modelData.configure_label.length > 0
                    text: modelData.configure_label
                    icon.name: "config-users"
                    onClicked: {
                        kcm.launchMcpConfigure(modelData.configure_command)
                        statusText = "Launched sign-in (finish in the browser, then Refresh)"
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

    QQC2.Dialog {
        id: removeDialog
        modal: true
        anchors.centerIn: parent
        title: "Remove service account"
        implicitWidth: 420
        standardButtons: QQC2.Dialog.Cancel | QQC2.Dialog.Ok

        property var target: null
        function promptFor(item) {
            target = item
            open()
        }

        onAccepted: {
            if (!target) return
            const id = target.id
            kcm.daemonCall("remove_service_account", { id: id }, function(result, error) {
                if (error) {
                    statusText = "Remove failed: " + error
                    return
                }
                statusText = "Removed service account '" + id + "'"
                reload()
            })
        }

        ColumnLayout {
            anchors.fill: parent
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: removeDialog.target
                    ? ("Remove service account '" + (removeDialog.target.display_name
                       || removeDialog.target.id) + "'? Servers that reference it will "
                       + "no longer authenticate until pointed at another account.")
                    : ""
            }
        }
    }

    ServiceAccountEditor {
        id: editor
        onDone: function(succeeded) {
            if (succeeded) {
                statusText = "Saved."
                reload()
            }
        }
    }
}
