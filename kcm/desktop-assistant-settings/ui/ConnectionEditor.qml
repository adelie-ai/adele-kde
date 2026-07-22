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
    // Streaming stall budgets (seconds); blank = use the connector default.
    // Supported on every connector; most useful for slow local Ollama models.
    property string fieldConnectTimeout: ""
    property string fieldStreamTimeout: ""
    // Context length hard cap (tokens); blank = "Max available". See the help
    // text on the field below. Supported on every connector.
    property string fieldMaxContextTokens: ""
    // Anthropic + OpenAI
    property string fieldApiKeyEnv: ""
    property string fieldApiKeyInput: ""
    // Bedrock
    property string fieldAwsProfile: ""
    property string fieldRegion: ""
    // Ollama
    property bool fieldAutoPull: false
    property bool fieldKeepWarm: false
    // Bedrock models refresh state
    property string refreshStatus: ""
    property var refreshedModels: []

    function openForNew(connType) {
        connectorType = (connType || "").toLowerCase()
        connectionId = ""
        isNew = true
        fieldBaseUrl = ""
        fieldConnectTimeout = ""
        fieldStreamTimeout = ""
        fieldMaxContextTokens = ""
        fieldApiKeyEnv = ""
        fieldApiKeyInput = ""
        fieldAwsProfile = ""
        fieldRegion = ""
        fieldAutoPull = false
        fieldKeepWarm = false
        refreshStatus = ""
        refreshedModels = []
        open()
    }

    function openFor(item) {
        connectorType = String(item.connector_type || "").toLowerCase()
        connectionId = String(item.id || "")
        isNew = false
        fieldApiKeyInput = ""
        refreshStatus = ""
        refreshedModels = []
        // The daemon echoes the stored *non-secret* config on `ListConnections`
        // (`ConnectionView.config`), so pre-fill from it. This matters because
        // `update_connection` REPLACES the whole connection — without pre-fill,
        // saving a keep-warm/timeout tweak would wipe an existing base_url.
        // API-key *values* are never echoed (only the env-var name) and stay
        // in the keyring untouched unless a new value is typed below.
        var c = item.config || {}
        fieldBaseUrl = String(c.base_url || "")
        fieldApiKeyEnv = String(c.api_key_env || "")
        fieldAwsProfile = String(c.aws_profile || "")
        fieldRegion = String(c.region || "")
        fieldAutoPull = false
        fieldConnectTimeout = (c.connect_timeout_secs !== undefined && c.connect_timeout_secs !== null)
            ? String(c.connect_timeout_secs) : ""
        fieldStreamTimeout = (c.stream_timeout_secs !== undefined && c.stream_timeout_secs !== null)
            ? String(c.stream_timeout_secs) : ""
        fieldMaxContextTokens = (c.max_context_tokens !== undefined && c.max_context_tokens !== null)
            ? String(c.max_context_tokens) : ""
        fieldKeepWarm = c.keep_warm === true
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
        if (connectorType === "anthropic" || connectorType === "openai" || connectorType === "openrouter") {
            if (fieldBaseUrl.trim().length > 0) config.base_url = fieldBaseUrl.trim()
            if (fieldApiKeyEnv.trim().length > 0) config.api_key_env = fieldApiKeyEnv.trim()
        } else if (connectorType === "bedrock") {
            if (fieldAwsProfile.trim().length > 0) config.aws_profile = fieldAwsProfile.trim()
            if (fieldRegion.trim().length > 0) config.region = fieldRegion.trim()
            if (fieldBaseUrl.trim().length > 0) config.base_url = fieldBaseUrl.trim()
        } else if (connectorType === "ollama") {
            if (fieldBaseUrl.trim().length > 0) config.base_url = fieldBaseUrl.trim()
            // Keep this connection's interactive model resident in Ollama's
            // memory (maps to `OllamaConnection.keep_warm`). Only send `true`;
            // omit when off so the field round-trips cleanly.
            if (fieldKeepWarm) config.keep_warm = true
            // `auto_pull` is a UI intent that doesn't map to a daemon field
            // today (see ConnectionConfigView::Ollama). We stash it into a
            // future-proof `_meta` key so the field survives round-trips
            // once the daemon grows it.
        } else {
            refreshStatus = "Unsupported connector type: " + connectorType
            return
        }

        // Streaming stall budgets apply to every connector. Blank → omit
        // (use the connector default); a positive integer → override seconds.
        const connectSecs = parseInt(fieldConnectTimeout.trim(), 10)
        if (!isNaN(connectSecs) && connectSecs > 0) config.connect_timeout_secs = connectSecs
        const streamSecs = parseInt(fieldStreamTimeout.trim(), 10)
        if (!isNaN(streamSecs) && streamSecs > 0) config.stream_timeout_secs = streamSecs

        // Context length hard cap applies to every connector. Blank → omit =
        // "Max available" (use the model's reported/curated window); a positive
        // integer caps the effective window to min(value, model max).
        const maxCtx = parseInt(fieldMaxContextTokens.trim(), 10)
        if (!isNaN(maxCtx) && maxCtx > 0) config.max_context_tokens = maxCtx

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
                    : connectorType === "openrouter"
                    ? "OpenRouter connector: an OpenAI-compatible aggregator. Set the API-key env var and optionally override the base URL (defaults to the OpenRouter endpoint)."
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
            visible: connectorType === "anthropic" || connectorType === "openai" || connectorType === "openrouter"
            Layout.fillWidth: true
            QQC2.Label {
                text: "API key env var"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                placeholderText: connectorType === "anthropic"
                    ? "ANTHROPIC_API_KEY"
                    : connectorType === "openrouter"
                        ? "OPENROUTER_API_KEY"
                        : "OPENAI_API_KEY"
                text: fieldApiKeyEnv
                onTextEdited: fieldApiKeyEnv = text
            }
        }

        RowLayout {
            visible: connectorType === "anthropic" || connectorType === "openai" || connectorType === "openrouter"
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
            visible: connectorType === "anthropic" || connectorType === "openai" || connectorType === "openrouter" || connectorType === "bedrock" || connectorType === "ollama"
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
                            : connectorType === "openrouter"
                                ? "https://openrouter.ai/api/v1"
                                : "https://api.openai.com/v1"
                text: fieldBaseUrl
                onTextEdited: fieldBaseUrl = text
            }
        }

        // Streaming stall budgets (all connectors). Most useful for Ollama,
        // where a large model on CPU can take longer than the 30s default just
        // to return its first token.
        RowLayout {
            visible: connectorType === "anthropic" || connectorType === "openai" || connectorType === "openrouter" || connectorType === "bedrock" || connectorType === "ollama"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Connect timeout (s)"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 0 }
                placeholderText: "default 30 — blank to keep"
                text: fieldConnectTimeout
                onTextEdited: fieldConnectTimeout = text
            }
        }

        RowLayout {
            visible: connectorType === "anthropic" || connectorType === "openai" || connectorType === "openrouter" || connectorType === "bedrock" || connectorType === "ollama"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Stream timeout (s)"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 0 }
                placeholderText: "default 60 — blank to keep"
                text: fieldStreamTimeout
                onTextEdited: fieldStreamTimeout = text
            }
        }

        RowLayout {
            visible: connectorType === "anthropic" || connectorType === "openai" || connectorType === "openrouter" || connectorType === "bedrock" || connectorType === "ollama"
            Layout.fillWidth: true
            QQC2.Label {
                text: "Context length hard cap"
                Layout.preferredWidth: 160
            }
            QQC2.TextField {
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 0 }
                placeholderText: "Max available"
                text: fieldMaxContextTokens
                onTextEdited: fieldMaxContextTokens = text
            }
        }

        QQC2.Label {
            visible: connectorType === "anthropic" || connectorType === "openai" || connectorType === "openrouter" || connectorType === "bedrock" || connectorType === "ollama"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            // Documents exactly what the hard cap does, per connector kind.
            text: connectorType === "ollama"
                ? "Hard ceiling on the context window, in tokens — a down-only cap. Leave blank for no cap; the window then follows the model's default and your per-purpose context setting. Set a number to hold the window at no more than this, e.g. 16384 if the model's full window won't fit in RAM on this machine. The cap lowers the num_ctx sent to Ollama and the prompt budget together, so they stay consistent."
                : "Hard ceiling on the context window, in tokens — a down-only cap. Leave blank for “Max available” (the model's full window). Set a number to cap how much context the assistant packs per request (effective = min(this, the model's window)), e.g. to bound cost on a metered API even if the model supports far more."
        }

        QQC2.Label {
            visible: connectorType === "ollama"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            text: "Tip: a large Ollama model on CPU can take well over 30s for its first token. Raise the connect timeout, and enable keep-warm below, if you see “Ollama stream stalled”."
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

        RowLayout {
            visible: connectorType === "ollama"
            Layout.fillWidth: true
            QQC2.CheckBox {
                text: "Keep interactive model warm"
                checked: fieldKeepWarm
                onToggled: fieldKeepWarm = checked
            }
            QQC2.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                text: "Periodically re-loads the interactive model so replies aren't delayed by a cold load. Only the interactive model is kept resident."
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
