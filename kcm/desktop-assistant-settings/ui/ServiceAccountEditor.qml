/*
 * Service-account editor (typed-OAuth / service-accounts epic, KCM side —
 * adelie-ai/desktop-assistant#477 / adele-kde#105).
 *
 * A reusable *outbound* OAuth credential (the thing Adele uses to reach a remote
 * service like Google Workspace). One account is referenced by many MCP servers,
 * so the OAuth client identity is entered once here instead of per server.
 *
 * SECURITY: the client-secret *value* never goes into mcp_servers.toml. When the
 * user types one we call set_mcp_secret(ref, value) FIRST, then
 * upsert_service_account with only the *ref* (client_secret_ref). The refresh
 * token is NEVER typed here — it is minted later by the row's Sign in button; we
 * only reserve a refresh_token_ref (<id>_refresh).
 *
 * Contract: ListServiceAccountsJson echoes the non-secret fields (id,
 * display_name, client_id, authorize_url, token_url, account, granted_scopes)
 * plus the refs, so editing an existing account pre-fills them and a save
 * round-trips without blanking them — UpsertServiceAccount replaces by id, so
 * anything left out is dropped. granted_scopes are preserved verbatim on edit
 * (they are owned by the sign-in flow, not this editor).
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.OverlaySheet {
    id: sheet

    // Fixed, parent-INDEPENDENT size. The other editors derive their size from
    // `parent` (`Math.min(parent.height - …, 720)`), which works only because
    // they are declared inside a full-height *page*. This sheet is declared inside
    // a short section (the Auth-tab Service Accounts block, and the "New account…"
    // flow inside the MCP editor), so `parent.height` was that section's ~72px and
    // the content collapsed to a sliver over the scrim — the "darkens but no UI"
    // bug. A fixed implicit size renders correctly wherever it is declared;
    // OverlaySheet still caps to the window and scrolls its content if too tall.
    implicitWidth: Kirigami.Units.gridUnit * 30
    implicitHeight: Kirigami.Units.gridUnit * 28

    signal done(bool succeeded)

    property bool isNew: false
    property string fieldId: ""
    property string fieldDisplayName: ""
    property string fieldClientId: ""
    property string fieldAuthorizeUrl: ""
    property string fieldTokenUrl: ""
    property string fieldAccount: ""
    property string fieldClientSecret: ""
    // Carried through unchanged on edit so a save never blanks them.
    property var fieldGrantedScopes: []
    property string existingClientSecretRef: ""
    property string existingRefreshTokenRef: ""
    property string errorText: ""

    function resetFields() {
        fieldDisplayName = ""
        fieldClientId = ""
        fieldAuthorizeUrl = "https://accounts.google.com/o/oauth2/v2/auth"
        fieldTokenUrl = "https://oauth2.googleapis.com/token"
        fieldAccount = ""
        fieldClientSecret = ""
        fieldGrantedScopes = []
        existingClientSecretRef = ""
        existingRefreshTokenRef = ""
        errorText = ""
    }

    function openForNew() {
        isNew = true
        fieldId = ""
        resetFields()
        open()
    }

    function openFor(item) {
        isNew = false
        resetFields()
        fieldId = String(item.id || "")
        fieldDisplayName = String(item.display_name || "")
        fieldClientId = String(item.client_id || "")
        fieldAuthorizeUrl = String(item.authorize_url || "")
        fieldTokenUrl = String(item.token_url || "")
        fieldAccount = String(item.account || "")
        fieldGrantedScopes = Array.isArray(item.granted_scopes) ? item.granted_scopes : []
        existingClientSecretRef = String(item.client_secret_ref || "")
        existingRefreshTokenRef = String(item.refresh_token_ref || "")
        open()
    }

    title: isNew ? "Add service account" : ("Edit " + (fieldDisplayName || fieldId))

    function saveAccount() {
        const id = fieldId.trim()
        if (id.length === 0) {
            errorText = "An id is required (a short handle, e.g. work-google)"
            return
        }
        const clientId = fieldClientId.trim()
        const authorizeUrl = fieldAuthorizeUrl.trim()
        const tokenUrl = fieldTokenUrl.trim()
        if (clientId.length === 0 || authorizeUrl.length === 0 || tokenUrl.length === 0) {
            errorText = "Client ID, Authorize URL and Token URL are required"
            return
        }

        const config = {
            id: id,
            client_id: clientId,
            authorize_url: authorizeUrl,
            token_url: tokenUrl,
            // Reserved now; minted by the row's Sign in button (never typed).
            // Preserve the existing ref on edit so a prior token isn't orphaned.
            refresh_token_ref: existingRefreshTokenRef.length > 0
                ? existingRefreshTokenRef : (id + "_refresh"),
            // Owned by the sign-in flow — carried through unchanged.
            granted_scopes: fieldGrantedScopes,
        }
        if (fieldDisplayName.trim().length > 0) config.display_name = fieldDisplayName.trim()
        if (fieldAccount.trim().length > 0) config.account = fieldAccount.trim()

        var pendingSecret = null
        if (fieldClientSecret.length > 0) {
            // Confidential client: store the value, reference it.
            const secretRef = existingClientSecretRef.length > 0
                ? existingClientSecretRef : (id + "_client_secret")
            config.client_secret_ref = secretRef
            pendingSecret = { id: secretRef, value: fieldClientSecret }
        } else if (existingClientSecretRef.length > 0) {
            // Edit with the field left blank: keep the stored secret's ref.
            config.client_secret_ref = existingClientSecretRef
        }
        // else: public (PKCE) client — no client_secret_ref at all.

        if (pendingSecret) {
            kcm.daemonCall("set_mcp_secret",
                { id: pendingSecret.id, value: pendingSecret.value },
                function(result, error) {
                    if (error) {
                        errorText = "Failed to store client secret: " + error
                        return
                    }
                    upsert(config)
                })
        } else {
            upsert(config)
        }
    }

    function upsert(config) {
        kcm.daemonCall("upsert_service_account", { config: config }, function(result, error) {
            if (error) {
                errorText = error
                return
            }
            close()
            done(true)
        })
    }

    ColumnLayout {
        spacing: 8

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "A reusable OAuth credential Adele uses to reach an external "
                + "service. Enter the client identity once; MCP servers then "
                + "reference this account instead of repeating it. The client "
                + "secret is stored securely and never kept in the config, and the "
                + "refresh token is minted by Sign in — never typed here."
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                text: "Id"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                enabled: sheet.isNew
                placeholderText: "work-google (stable handle servers reference)"
                text: fieldId
                onTextEdited: fieldId = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                text: "Display name"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "Work Google Workspace"
                text: fieldDisplayName
                onTextEdited: fieldDisplayName = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                text: "Client ID"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "1234.apps.googleusercontent.com"
                text: fieldClientId
                onTextEdited: fieldClientId = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                text: "Client secret"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: sheet.isNew
                    ? "optional — blank for a public (PKCE) client"
                    : "leave blank to keep the stored secret"
                text: fieldClientSecret
                onTextEdited: fieldClientSecret = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                text: "Authorize URL"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "https://accounts.google.com/o/oauth2/v2/auth"
                text: fieldAuthorizeUrl
                onTextEdited: fieldAuthorizeUrl = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                text: "Token URL"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "https://oauth2.googleapis.com/token"
                text: fieldTokenUrl
                onTextEdited: fieldTokenUrl = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                text: "Account"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "optional — token-store key, e.g. user@example.com"
                text: fieldAccount
                onTextEdited: fieldAccount = text
            }
        }

        QQC2.Label {
            visible: fieldGrantedScopes.length > 0
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            text: "Granted scopes: " + fieldGrantedScopes.join("  ")
        }

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            text: "After saving, click Sign in on this account's row to open the "
                + "browser and mint the refresh token. Scopes are collected from "
                + "the servers that reference this account."
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                Layout.fillWidth: true
                text: errorText
                color: Kirigami.Theme.negativeTextColor
                wrapMode: Text.Wrap
                visible: errorText.length > 0
            }
            QQC2.Button {
                text: "Cancel"
                onClicked: {
                    close()
                    done(false)
                }
            }
            QQC2.Button {
                text: isNew ? "Create" : "Save"
                highlighted: true
                onClicked: saveAccount()
            }
        }
    }
}
