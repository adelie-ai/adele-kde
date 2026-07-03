/*
 * Transport-aware MCP server editor (mcp-servers-ui epic, KCM side).
 *
 * The daemon's `McpServerConfig` spans two transports with divergent field
 * sets, so — mirroring ConnectionEditor.qml's per-variant `visible:` bindings —
 * we pick the form from a Transport selector:
 *
 *   Local (stdio):  command + args + namespace + env.
 *   Remote (HTTP):  url + an Authentication sub-selector (None / Bearer / OAuth).
 *                     Bearer: a token value → stored via SetMcpSecret, config
 *                             carries only the secret *ref* (auth_bearer_secret).
 *                     OAuth:  client_id / token_url / authorize_url / scopes /
 *                             account (+ optional client secret value → SetMcpSecret).
 *                             The refresh token is NEVER typed here — it is minted
 *                             later by the row's Sign in button; we only reserve a
 *                             refresh_token_ref (<name>_refresh).
 *
 * SECURITY: secret *values* never go into config_json. Where the user enters a
 * value (bearer token, OAuth client secret) we call set_mcp_secret(ref, value)
 * FIRST, then upsert_mcp_server with the ref only.
 *
 * Note (contract): ListMcpServersJson echoes only the OAuth *summary* (authorized
 * / account / scopes), not client_id / token_url / authorize_url / refresh or
 * client-secret refs. So editing an existing remote server pre-fills what the
 * daemon reports and leaves those request fields blank to re-enter — UpsertMcpServer
 * replaces the server by name, so anything left blank is dropped.
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.OverlaySheet {
    id: sheet

    // Size the sheet explicitly. Without this, OverlaySheet picks an implicit
    // width from the inner ColumnLayout's children, which collapses to the
    // longest label and renders a tiny popup (see ConnectionEditor.qml).
    implicitWidth: Math.min(parent ? parent.width - Kirigami.Units.gridUnit * 2 : 560, 720)
    implicitHeight: Math.min(parent ? parent.height - Kirigami.Units.gridUnit * 2 : 600, 720)

    signal done(bool succeeded)

    property bool isNew: false
    // Source of truth for the divergent forms; the selectors below write these
    // and the field rows key their `visible:` off them.
    property string transport: "stdio"   // "stdio" | "http"
    property string authKind: "none"     // "none" | "bearer" | "oauth"

    property string serverName: ""
    property bool fieldEnabled: true

    // stdio
    property string fieldCommand: ""
    property string fieldArgs: ""
    property string fieldNamespace: ""
    property string fieldEnv: ""

    // http (shared)
    property string fieldUrl: ""
    // http + bearer
    property string fieldBearerToken: ""
    // http + oauth
    property string fieldClientId: ""
    property string fieldTokenUrl: ""
    property string fieldAuthorizeUrl: ""
    property string fieldScopes: ""
    property string fieldAccount: ""
    property string fieldClientSecret: ""

    property string errorText: ""

    function resetFields() {
        fieldEnabled = true
        fieldCommand = ""
        fieldArgs = ""
        fieldNamespace = ""
        fieldEnv = ""
        fieldUrl = ""
        fieldBearerToken = ""
        fieldClientId = ""
        fieldTokenUrl = ""
        fieldAuthorizeUrl = ""
        fieldScopes = ""
        fieldAccount = ""
        fieldClientSecret = ""
        errorText = ""
    }

    function openForNew() {
        isNew = true
        serverName = ""
        transport = "stdio"
        authKind = "none"
        transportCombo.currentIndex = 0
        authCombo.currentIndex = 0
        resetFields()
        open()
    }

    function openFor(item) {
        isNew = false
        serverName = String(item.name || "")
        transport = String(item.transport || "stdio")
        transportCombo.currentIndex = (transport === "http") ? 1 : 0
        resetFields()

        fieldEnabled = item.enabled !== false
        // stdio pre-fill (also carries `command` for a stdio server; `target`
        // is the human-display command/url).
        fieldCommand = String(item.command || "")
        fieldArgs = (Array.isArray(item.args) ? item.args : []).join(" ")
        fieldNamespace = String(item.namespace || "")

        // remote pre-fill: url comes back as `target` for an http server. Only
        // the OAuth *summary* (kind / account / scopes) is echoed — client_id,
        // token_url, authorize_url and the secret refs are not, so leave them
        // blank to re-enter (see the header note).
        if (transport === "http") {
            fieldUrl = String(item.target || "")
            authKind = String(item.auth_kind || "none")
            authCombo.currentIndex = (authKind === "bearer") ? 1 : (authKind === "oauth" ? 2 : 0)
            fieldAccount = String(item.oauth_account || "")
            fieldScopes = (Array.isArray(item.oauth_scopes) ? item.oauth_scopes : []).join(" ")
        } else {
            authKind = "none"
            authCombo.currentIndex = 0
        }
        open()
    }

    title: isNew ? "Add MCP server" : ("Edit " + serverName)

    // Split whitespace-separated args/scopes into a clean array (drops empties).
    function splitWords(text) {
        const parts = text.trim().split(/\s+/)
        const out = []
        for (let i = 0; i < parts.length; i++) {
            if (parts[i].length > 0) out.push(parts[i])
        }
        return out
    }

    // Parse KEY=value lines into an env object. Blank lines and lines without an
    // '=' are ignored; only the first '=' splits so values may contain '='.
    function parseEnv(text) {
        const env = ({})
        const lines = text.split(/\n/)
        for (let i = 0; i < lines.length; i++) {
            const line = lines[i].trim()
            if (line.length === 0) continue
            const eq = line.indexOf("=")
            if (eq <= 0) continue
            const key = line.substring(0, eq).trim()
            const value = line.substring(eq + 1).trim()
            if (key.length > 0) env[key] = value
        }
        return env
    }

    function saveServer() {
        const name = serverName.trim()
        if (name.length === 0) {
            errorText = "Server name is required"
            return
        }

        const config = { name: name, enabled: fieldEnabled }
        // A secret value to write (bearer token / OAuth client secret) before the
        // upsert, or null. Only ever one at a time given the current auth kinds.
        var pendingSecret = null

        if (transport === "stdio") {
            const command = fieldCommand.trim()
            if (command.length === 0) {
                errorText = "Command is required for a local (stdio) server"
                return
            }
            config.command = command
            const args = splitWords(fieldArgs)
            if (args.length > 0) config.args = args
            if (fieldNamespace.trim().length > 0) config.namespace = fieldNamespace.trim()
            const env = parseEnv(fieldEnv)
            if (Object.keys(env).length > 0) config.env = env
        } else if (transport === "http") {
            const url = fieldUrl.trim()
            if (url.length === 0) {
                errorText = "URL is required for a remote (HTTP) server"
                return
            }
            const http = { url: url }
            if (authKind === "bearer") {
                // Deterministic ref so the value round-trips; only (re)write the
                // value when the user actually typed one this time.
                const bearerRef = name + "_token"
                http.auth_bearer_secret = bearerRef
                if (fieldBearerToken.length > 0) {
                    pendingSecret = { id: bearerRef, value: fieldBearerToken }
                }
            } else if (authKind === "oauth") {
                const clientId = fieldClientId.trim()
                const tokenUrl = fieldTokenUrl.trim()
                const authorizeUrl = fieldAuthorizeUrl.trim()
                if (clientId.length === 0 || tokenUrl.length === 0 || authorizeUrl.length === 0) {
                    errorText = "OAuth needs client id, token URL and authorize URL"
                    return
                }
                const oauth = {
                    client_id: clientId,
                    token_url: tokenUrl,
                    authorize_url: authorizeUrl,
                    // Reserved now, minted by the row's Sign in button (never typed).
                    refresh_token_ref: name + "_refresh",
                    scopes: splitWords(fieldScopes),
                }
                if (fieldAccount.trim().length > 0) oauth.account = fieldAccount.trim()
                // Confidential client: store the secret value and reference it.
                // Blank → PKCE-public (omit the ref entirely).
                if (fieldClientSecret.length > 0) {
                    const secretRef = name + "_client_secret"
                    oauth.client_secret_ref = secretRef
                    pendingSecret = { id: secretRef, value: fieldClientSecret }
                }
                http.oauth = oauth
            }
            config.http = http
        } else {
            errorText = "Unsupported transport: " + transport
            return
        }

        // Store the secret value FIRST (the daemon reloads secrets.toml so the
        // following upsert resolves the ref), then upsert. No secret → upsert now.
        if (pendingSecret) {
            kcm.daemonCall("set_mcp_secret",
                { id: pendingSecret.id, value: pendingSecret.value },
                function(result, error) {
                    if (error) {
                        errorText = "Failed to store secret: " + error
                        return
                    }
                    upsert(config)
                })
        } else {
            upsert(config)
        }
    }

    function upsert(config) {
        kcm.daemonCall("upsert_mcp_server", { config: config }, function(result, error) {
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
            text: "Configure an MCP server. Local servers run a stdio command; "
                + "remote servers connect to an HTTP endpoint with optional bearer "
                + "or OAuth authentication. Secret values are stored securely and "
                + "never kept in the server config."
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                text: "Name"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                enabled: sheet.isNew
                placeholderText: "gmail-work, weather, my-server…"
                text: serverName
                onTextEdited: serverName = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                text: "Transport"
                Layout.preferredWidth: 160
            }
            QQC2.ComboBox {
                id: transportCombo
                Layout.fillWidth: true
                model: ["Local (stdio)", "Remote (HTTP)"]
                onActivated: transport = (currentIndex === 1 ? "http" : "stdio")
            }
        }

        QQC2.CheckBox {
            text: "Enabled"
            checked: fieldEnabled
            onToggled: fieldEnabled = checked
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        // --- Local (stdio) fields ------------------------------------------

        RowLayout {
            visible: transport === "stdio"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Command"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "/usr/bin/weather-mcp"
                text: fieldCommand
                onTextEdited: fieldCommand = text
            }
        }

        RowLayout {
            visible: transport === "stdio"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Arguments"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "--port 8080 --verbose (space-separated)"
                text: fieldArgs
                onTextEdited: fieldArgs = text
            }
        }

        RowLayout {
            visible: transport === "stdio"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Namespace"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "optional — prefixes this server's tool names"
                text: fieldNamespace
                onTextEdited: fieldNamespace = text
            }
        }

        ColumnLayout {
            visible: transport === "stdio"
            Layout.fillWidth: true
            spacing: 2
            QQC2.Label {
                text: "Environment (one KEY=value per line)"
            }
            QQC2.TextArea {
                Layout.fillWidth: true
                Layout.minimumHeight: 72
                wrapMode: TextEdit.NoWrap
                placeholderText: "LOG=info\nREGION=us-east-1"
                text: fieldEnv
                onTextChanged: fieldEnv = text
            }
        }

        // --- Remote (HTTP) fields ------------------------------------------

        RowLayout {
            visible: transport === "http"
            Layout.fillWidth: true
            QQC2.Label {
                text: "URL"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "https://example.com/mcp"
                text: fieldUrl
                onTextEdited: fieldUrl = text
            }
        }

        RowLayout {
            visible: transport === "http"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Authentication"
                Layout.preferredWidth: 160
            }
            QQC2.ComboBox {
                id: authCombo
                Layout.fillWidth: true
                model: ["None", "Bearer token", "OAuth"]
                onActivated: authKind = (currentIndex === 1 ? "bearer" : (currentIndex === 2 ? "oauth" : "none"))
            }
        }

        // Bearer
        RowLayout {
            visible: transport === "http" && authKind === "bearer"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Bearer token"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: sheet.isNew
                    ? "Stored in secrets.toml (write-only)"
                    : "Leave blank to keep the stored token"
                text: fieldBearerToken
                onTextEdited: fieldBearerToken = text
            }
        }

        // OAuth
        RowLayout {
            visible: transport === "http" && authKind === "oauth"
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
            visible: transport === "http" && authKind === "oauth"
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
            visible: transport === "http" && authKind === "oauth"
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
            visible: transport === "http" && authKind === "oauth"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Scopes"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "space-separated, e.g. https://www.googleapis.com/auth/gmail.modify"
                text: fieldScopes
                onTextEdited: fieldScopes = text
            }
        }

        RowLayout {
            visible: transport === "http" && authKind === "oauth"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Account"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "optional — token-store key, e.g. dave@example.com"
                text: fieldAccount
                onTextEdited: fieldAccount = text
            }
        }

        RowLayout {
            visible: transport === "http" && authKind === "oauth"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Client secret"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: "optional — blank for a public (PKCE) client"
                text: fieldClientSecret
                onTextEdited: fieldClientSecret = text
            }
        }

        QQC2.Label {
            visible: transport === "http" && authKind === "oauth"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            text: "After saving, this server shows “Sign in required”. Click Sign in on "
                + "its row to open the browser and mint the refresh token — it is never "
                + "typed here."
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
                onClicked: saveServer()
            }
        }
    }
}
