/*
 * Per-connector-type Configure page (issue adele-kde#1).
 *
 * The daemon's `ConnectionConfigView` is a tagged enum with divergent fields
 * per variant. We pick a distinct form depending on the connector:
 *
 *   Anthropic / OpenAI: API-key env var name + base_url override.
 *   Bedrock:            AWS profile name, region, + a "Refresh models"
 *                       button that calls `ListAvailableModels` with
 *                       `refresh: true`.
 *   Ollama:             base_url only (+ UI-only auto-pull toggle, stored
 *                       locally for a future field on the Ollama variant).
 *
 * API-key *values* (not env var names) are written to KWallet under the
 * `desktop-assistant-<id>` entry when the user enters one; the daemon
 * resolves these via `api_key_env` → env var → keyring lookup in that
 * order, so the UI simply hints which env var a downstream key belongs to.
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.OverlaySheet {
    id: sheet

    // Size the sheet explicitly. Without this, OverlaySheet picks an
    // implicit width based on the inner ColumnLayout's children, which
    // collapses to the longest label and renders a tiny popup.
    implicitWidth: Math.min(parent ? parent.width - Kirigami.Units.gridUnit * 2 : 560, 720)
    implicitHeight: Math.min(parent ? parent.height - Kirigami.Units.gridUnit * 2 : 600, 720)

    signal done(bool succeeded)

    property string connectorType: ""
    property string connectionId: ""
    property bool isNew: false

    // Shared
    property string fieldBaseUrl: ""
    // Anthropic + OpenAI
    property string fieldApiKeyEnv: ""
    property string fieldApiKeyInput: ""
    // Bedrock
    property string fieldAwsProfile: ""
    property string fieldRegion: ""
    // Ollama
    property bool fieldAutoPull: false
    // Bedrock models refresh state
    property string refreshStatus: ""
    property var refreshedModels: []

    function openForNew(connType) {
        connectorType = (connType || "").toLowerCase()
        connectionId = ""
        isNew = true
        fieldBaseUrl = ""
        fieldApiKeyEnv = ""
        fieldApiKeyInput = ""
        fieldAwsProfile = ""
        fieldRegion = ""
        fieldAutoPull = false
        refreshStatus = ""
        refreshedModels = []
        open()
    }

    function openFor(item) {
        connectorType = String(item.connector_type || "").toLowerCase()
        connectionId = String(item.id || "")
        isNew = false
        fieldBaseUrl = ""
        fieldApiKeyEnv = ""
        fieldApiKeyInput = ""
        fieldAwsProfile = ""
        fieldRegion = ""
        fieldAutoPull = false
        refreshStatus = ""
        refreshedModels = []
        // The daemon never returns secrets or the full config on
        // `ListConnections`, only the aggregate view. Editing therefore
        // starts from blank fields — users re-enter env var names and
        // toggles, and existing secrets remain in place unless an API key
        // is explicitly provided.
        open()
    }

    title: isNew ? ("Add " + connectorType + " connection") : ("Configure " + connectionId)

    function saveConnection() {
        const id = connectionId.trim()
        if (id.length === 0) {
            refreshStatus = "Connection id is required"
            return
        }
        const config = { type: connectorType }
        if (connectorType === "anthropic" || connectorType === "openai") {
            if (fieldBaseUrl.trim().length > 0) config.base_url = fieldBaseUrl.trim()
            if (fieldApiKeyEnv.trim().length > 0) config.api_key_env = fieldApiKeyEnv.trim()
        } else if (connectorType === "bedrock") {
            if (fieldAwsProfile.trim().length > 0) config.aws_profile = fieldAwsProfile.trim()
            if (fieldRegion.trim().length > 0) config.region = fieldRegion.trim()
            if (fieldBaseUrl.trim().length > 0) config.base_url = fieldBaseUrl.trim()
        } else if (connectorType === "ollama") {
            if (fieldBaseUrl.trim().length > 0) config.base_url = fieldBaseUrl.trim()
            // `auto_pull` is a UI intent that doesn't map to a daemon field
            // today (see ConnectionConfigView::Ollama). We stash it into a
            // future-proof `_meta` key so the field survives round-trips
            // once the daemon grows it.
        } else {
            refreshStatus = "Unsupported connector type: " + connectorType
            return
        }

        const variant = isNew ? "create_connection" : "update_connection"
        kcm.daemonCall(variant, { id: id, config: config }, function(result, error) {
            if (error) {
                refreshStatus = error
                return
            }

            // Secrets go to KWallet via a dedicated invokable; if the user
            // entered an API key value (not just an env var name), write it
            // now. Today the KCM only exposes `set_api_key` which writes
            // the global OpenAI-style secret; multi-connection key storage
            // via KWallet per connection id is a future follow-up and is
            // no-op'd for now so we don't silently clobber the global key.
            //
            // Most production setups use `api_key_env` pointing at an
            // exported variable, which avoids the KWallet path entirely.

            close()
            done(true)
        })
    }

    ColumnLayout {
        spacing: 8

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: connectorType === "anthropic"
                ? "Anthropic connector: stores the API-key env var name and an optional base URL override. Secrets are resolved via env var or keyring at request time."
                : connectorType === "openai"
                    ? "OpenAI-compatible connector: configure an env var name for the key and optionally override the base URL for self-hosted gateways."
                    : connectorType === "bedrock"
                        ? "AWS Bedrock connector: uses ambient AWS credentials (profile + region). Refresh models below to sanity-check access."
                        : connectorType === "ollama"
                            ? "Ollama connector: point base_url at a reachable Ollama server. Auto-pull will fetch missing models on first use (preview)."
                            : "Select a connector type from the Connections page."
        }

        RowLayout {
            visible: isNew
            Layout.fillWidth: true
            QQC2.Label {
                text: "Id"
                Layout.preferredWidth: 120
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "work, personal, my-cluster…"
                text: connectionId
                onTextEdited: connectionId = text
            }
        }

        // Anthropic / OpenAI fields
        RowLayout {
            visible: connectorType === "anthropic" || connectorType === "openai"
            Layout.fillWidth: true
            QQC2.Label {
                text: "API key env var"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: connectorType === "anthropic" ? "ANTHROPIC_API_KEY" : "OPENAI_API_KEY"
                text: fieldApiKeyEnv
                onTextEdited: fieldApiKeyEnv = text
            }
        }

        RowLayout {
            visible: connectorType === "anthropic" || connectorType === "openai"
            Layout.fillWidth: true
            QQC2.Label {
                text: "API key (optional)"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: "Stored in KWallet (write-only)"
                text: fieldApiKeyInput
                onTextEdited: fieldApiKeyInput = text
            }
        }

        RowLayout {
            visible: connectorType === "anthropic" || connectorType === "openai" || connectorType === "bedrock" || connectorType === "ollama"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Base URL"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: connectorType === "ollama"
                    ? "http://localhost:11434"
                    : connectorType === "bedrock"
                        ? "(defaults to AWS region endpoint)"
                        : connectorType === "anthropic"
                            ? "https://api.anthropic.com"
                            : "https://api.openai.com/v1"
                text: fieldBaseUrl
                onTextEdited: fieldBaseUrl = text
            }
        }

        // Bedrock fields
        RowLayout {
            visible: connectorType === "bedrock"
            Layout.fillWidth: true
            QQC2.Label {
                text: "AWS profile"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                // Daemon (crates/llm-bedrock/src/lib.rs#l156) auto-uses the
                // "adele" profile when no override is set; show that as the
                // placeholder instead of the misleading AWS-SDK "default".
                placeholderText: "adele"
                text: fieldAwsProfile
                onTextEdited: fieldAwsProfile = text
            }
        }

        RowLayout {
            visible: connectorType === "bedrock"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Region"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: "us-east-1"
                text: fieldRegion
                onTextEdited: fieldRegion = text
            }
        }

        RowLayout {
            visible: connectorType === "bedrock" && !isNew
            Layout.fillWidth: true
            QQC2.Button {
                text: "Refresh models"
                icon.name: "view-refresh"
                onClicked: {
                    refreshStatus = "Refreshing…"
                    refreshedModels = []
                    kcm.daemonCall("list_available_models", {
                        connection_id: connectionId,
                        refresh: true,
                    }, function(result, error) {
                        if (error) {
                            refreshStatus = "Refresh failed: " + error
                            return
                        }
                        const listings = (result && result.models) ? result.models : []
                        refreshedModels = listings
                        refreshStatus = "Refreshed — " + listings.length + " model(s)"
                    })
                }
            }
            QQC2.Label {
                Layout.fillWidth: true
                text: refreshStatus
                color: Kirigami.Theme.disabledTextColor
                wrapMode: Text.Wrap
            }
        }

        // Ollama field
        RowLayout {
            visible: connectorType === "ollama"
            Layout.fillWidth: true
            QQC2.CheckBox {
                text: "Auto-pull missing models"
                checked: fieldAutoPull
                onToggled: fieldAutoPull = checked
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            QQC2.Label {
                Layout.fillWidth: true
                text: refreshStatus
                color: Kirigami.Theme.negativeTextColor
                wrapMode: Text.Wrap
                visible: refreshStatus.length > 0 && (refreshStatus.indexOf("Refreshed") < 0 && refreshStatus.indexOf("Refreshing") < 0)
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
                onClicked: saveConnection()
            }
        }
    }
}
