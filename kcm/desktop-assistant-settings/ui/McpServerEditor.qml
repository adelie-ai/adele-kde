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
 *                     OAuth:  a *service-account picker* (epic #477) + scopes. The
 *                             server references an account by id (oauth_account);
 *                             the OAuth client identity / secret / URLs live on the
 *                             account (managed on the Auth tab), and the refresh
 *                             token is minted by the account's Sign in there. The
 *                             picker is type-constrained: it lists service accounts
 *                             ONLY, never inbound WS/OIDC API-auth configs.
 *
 * SECURITY: no secret value is entered here for OAuth — the server carries only
 * an account *reference* + its required scopes. (Bearer still stores its token
 * value via set_mcp_secret first, then upserts the ref only.)
 *
 * Note (contract): ListMcpServersJson echoes `oauth_account_ref` (the referenced
 * account id) + `oauth_scopes`, so editing a referencing server pre-selects its
 * account and pre-fills scopes; a save round-trips the reference (no inline oauth
 * is written). A legacy inline-oauth server has no ref, so the picker starts
 * empty and the user selects an account to migrate it to.
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
    // http + oauth: the server contributes only its required scopes and
    // *references* a reusable service account (epic #477) — the OAuth client
    // identity (client_id / secret / urls) lives on the account, entered once.
    property string fieldScopes: ""
    property var serviceAccounts: []        // loaded from list_service_accounts
    property string selectedAccountId: ""   // the referenced account id, or ""

    property string errorText: ""

    function resetFields() {
        fieldEnabled = true
        fieldCommand = ""
        fieldArgs = ""
        fieldNamespace = ""
        fieldEnv = ""
        fieldUrl = ""
        fieldBearerToken = ""
        fieldScopes = ""
        selectedAccountId = ""
        errorText = ""
    }

    // Load the service accounts for the OAuth picker; type-constrained to
    // service accounts only (it never lists inbound WS/OIDC API-auth configs).
    // `preselectId` re-selects an account after a reload (e.g. a just-created one).
    function loadAccounts(preselectId) {
        kcm.daemonCall("list_service_accounts", {}, function(result, error) {
            if (error) {
                errorText = "Failed to load service accounts: " + error
                return
            }
            let rows = []
            if (typeof result === "string") {
                try { rows = JSON.parse(result) } catch (e) { rows = [] }
            } else if (result && result.accounts !== undefined) {
                rows = result.accounts
            } else if (result && result.length !== undefined) {
                rows = result
            }
            if (!rows || rows.length === undefined) { rows = [] }
            const list = []
            for (let i = 0; i < rows.length; i++) {
                const a = rows[i] || {}
                const id = String(a.id || "")
                if (id.length === 0) continue
                list.push({
                    id: id,
                    label: (String(a.display_name || "").length > 0
                        ? String(a.display_name) : id)
                        + (a.authorized ? "" : "  (not signed in)"),
                })
            }
            serviceAccounts = list
            if (preselectId !== undefined && preselectId.length > 0) {
                selectedAccountId = preselectId
            }
            syncAccountCombo()
        })
    }

    // Point the ComboBox at the currently-selected account id.
    function syncAccountCombo() {
        for (let i = 0; i < serviceAccounts.length; i++) {
            if (serviceAccounts[i].id === selectedAccountId) {
                accountCombo.currentIndex = i
                return
            }
        }
        accountCombo.currentIndex = -1
    }

    function openForNew() {
        isNew = true
        serverName = ""
        transport = "stdio"
        authKind = "none"
        transportCombo.currentIndex = 0
        authCombo.currentIndex = 0
        resetFields()
        loadAccounts()
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

        // remote pre-fill: url comes back as `target` for an http server. The
        // OAuth non-secret request fields (client_id / token_url / authorize_url
        // / account / scopes) are echoed too, so pre-fill them and the save
        // round-trips without blanking them. They are absent for non-oauth
        // servers, so `|| ""` guards undefined. Secret *values* are never echoed.
        if (transport === "http") {
            fieldUrl = String(item.target || "")
            authKind = String(item.auth_kind || "none")
            authCombo.currentIndex = (authKind === "bearer") ? 1 : (authKind === "oauth" ? 2 : 0)
            fieldScopes = (Array.isArray(item.oauth_scopes) ? item.oauth_scopes : []).join(" ")
            // Preselect the referenced service account (epic #477). Legacy
            // inline-oauth servers have no ref, so the picker starts empty and
            // the user picks an account to migrate to on save.
            selectedAccountId = String(item.oauth_account_ref || "")
            loadAccounts(selectedAccountId)
        } else {
            authKind = "none"
            authCombo.currentIndex = 0
            loadAccounts()
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
                // Reference a reusable service account (epic #477): the server
                // contributes only its required scopes + the account id. The
                // OAuth client identity + secret + refresh token live on the
                // account, so nothing secret is written here.
                if (selectedAccountId.length === 0) {
                    errorText = "Select a service account (or create one) for OAuth"
                    return
                }
                http.oauth_account = selectedAccountId
                const scopes = splitWords(fieldScopes)
                if (scopes.length > 0) http.scopes = scopes
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

        // OAuth: pick a reusable service account (epic #477) instead of
        // re-entering the client identity. The picker lists service accounts
        // ONLY — never inbound WS/OIDC API-auth configs (type-constrained).
        RowLayout {
            visible: transport === "http" && authKind === "oauth"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Service account"
                Layout.preferredWidth: 160
            }
            QQC2.ComboBox {
                id: accountCombo
                Layout.fillWidth: true
                textRole: "label"
                valueRole: "id"
                model: serviceAccounts
                displayText: currentIndex >= 0 ? currentText : "Select an account…"
                onActivated: selectedAccountId = serviceAccounts[currentIndex].id
            }
            QQC2.Button {
                text: "New account…"
                icon.name: "list-add"
                onClicked: accountEditor.openForNew()
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

        QQC2.Label {
            visible: transport === "http" && authKind === "oauth"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            text: "This server uses the selected account's OAuth client. Manage the "
                + "client id / secret / URLs and sign in from the Authentication "
                + "tab — one sign-in serves every server that shares the account."
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

        // "New account…" opens the same account editor used on the Auth tab;
        // on success we reload the picker and select the just-created account.
        ServiceAccountEditor {
            id: accountEditor
            onDone: function(succeeded) {
                if (succeeded) {
                    loadAccounts(accountEditor.fieldId)
                }
            }
        }
    }
}
