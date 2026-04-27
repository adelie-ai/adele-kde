/*
 * Purposes page (issue adele-kde#1).
 *
 * Shows one row per purpose — interactive / dreaming / embedding / titling —
 * and lets the user bind each to a `(connection, model, effort)` tuple via
 * the daemon's `GetPurposes` / `SetPurpose` commands. Connection and model
 * dropdowns are populated from `ListConnections` and `ListAvailableModels`.
 *
 * Non-interactive purposes can inherit from `interactive` by picking the
 * special string `"primary"` in the connection/model combobox — the daemon
 * resolves inheritance before dispatch (see desktop-assistant
 * crates/daemon/src/purposes.rs).
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root
    spacing: 10

    property string statusText: ""
    property var purposes: []
    property var connections: []
    property var modelsByConnection: ({})
    property bool loadingPurposes: false
    property bool loadingConnections: false

    // Keys are the purpose slugs; mirror the daemon's PurposeKindApi. Order
    // matters in the UI so we walk this list rather than iterating over
    // the response object directly.
    readonly property var purposeOrder: [
        { key: "interactive", label: "Interactive (chat)" },
        { key: "titling", label: "Titling (conversation names)" },
        { key: "dreaming", label: "Dreaming (background memory)" },
        { key: "embedding", label: "Embedding (semantic search)" },
    ]

    function reload() {
        loadingPurposes = true
        loadingConnections = true

        kcm.wsCall("list_connections", {}, function(result, error) {
            loadingConnections = false
            if (error) {
                statusText = error
                return
            }
            const rows = (result && result.connections) ? result.connections : []
            const slim = []
            for (let i = 0; i < rows.length; i++) {
                slim.push({
                    id: String(rows[i].id || ""),
                    label: String(rows[i].display_label || rows[i].id || ""),
                })
            }
            connections = slim
            reloadModels()
        })

        kcm.wsCall("get_purposes", {}, function(result, error) {
            loadingPurposes = false
            if (error) {
                statusText = error
                return
            }
            const view = result && result.purposes ? result.purposes : {}
            // The daemon protocol still understands the "primary" sentinel
            // for inheritance, but the UI now shows everything as explicit
            // values. Resolve any inherited entries to interactive's actual
            // connection/model on load so the dropdowns render real ids and
            // saving back rewrites them as explicit (no more "primary" on
            // the wire from this client).
            const interactive = view["interactive"] || {}
            const interactiveConn = String(interactive.connection || "")
            const interactiveModel = String(interactive.model || "")
            const out = []
            for (let i = 0; i < purposeOrder.length; i++) {
                const entry = purposeOrder[i]
                const cfg = view[entry.key] || {}
                let conn = String(cfg.connection || "")
                let model = String(cfg.model || "")
                if (entry.key !== "interactive") {
                    if (conn === "primary" || conn === "") conn = interactiveConn
                    if (model === "primary" || model === "") model = interactiveModel
                }
                out.push({
                    key: entry.key,
                    label: entry.label,
                    connection: conn,
                    model: model,
                    effort: cfg.effort ? String(cfg.effort) : "",
                })
            }
            purposes = out
        })
    }

    function reloadModels() {
        // Fetch models once across all connections; the per-connection map
        // lets the UI filter the Model combo as the Connection combo flips.
        kcm.wsCall("list_available_models", {}, function(result, error) {
            if (error) {
                statusText = error
                return
            }
            const listings = (result && result.models) ? result.models : []
            const byConn = ({})
            for (let i = 0; i < listings.length; i++) {
                const entry = listings[i] || {}
                const cid = String(entry.connection_id || "")
                if (cid.length === 0) continue
                if (!byConn[cid]) byConn[cid] = []
                const modelObj = entry.model || {}
                const caps = modelObj.capabilities || {}
                byConn[cid].push({
                    id: String(modelObj.id || ""),
                    display_name: String(modelObj.display_name || modelObj.id || ""),
                    embedding: Boolean(caps.embedding),
                })
            }
            modelsByConnection = byConn
        })
    }

    function persist(index) {
        const item = purposes[index]
        if (!item) return
        if (!item.connection || !item.model) return
        const config = {
            connection: item.connection,
            model: item.model,
        }
        if (item.effort && item.effort.length > 0) {
            config.effort = item.effort
        }

        kcm.wsCall("set_purpose", { purpose: item.key, config: config }, function(_result, error) {
            if (error) {
                statusText = "Failed to save purpose '" + item.key + "': " + error
                return
            }
            statusText = "Updated purpose '" + item.key + "'."
        })
    }

    Component.onCompleted: reload()

    // Refresh whenever the user flips back to this tab — connections added
    // or removed under the Connections tab won't have triggered our
    // list_connections call, so the dropdown would otherwise stay stale
    // until Apply.
    onVisibleChanged: if (visible) reload()

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: statusText.length > 0
        text: statusText
        type: Kirigami.MessageType.Information
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: "Route each assistant purpose through a specific connection and model."
    }

    QQC2.ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true

        ColumnLayout {
            width: parent ? parent.width : implicitWidth
            spacing: 6

            Repeater {
                model: purposes
                delegate: Kirigami.AbstractCard {
                    Layout.fillWidth: true

                    contentItem: ColumnLayout {
                        spacing: 4

                        QQC2.Label {
                            text: modelData.label
                            font.bold: true
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            QQC2.ComboBox {
                                Layout.fillWidth: true
                                textRole: "label"
                                model: {
                                    const base = []
                                    for (let i = 0; i < connections.length; i++) {
                                        base.push({ value: connections[i].id, label: connections[i].label })
                                    }
                                    // Always include the saved value so the
                                    // dropdown reflects the daemon state even
                                    // before list_connections completes (or
                                    // when the connection has been deleted
                                    // out from under this purpose).
                                    const cur = modelData.connection
                                    if (cur
                                        && !base.some(function(m) { return m.value === cur })) {
                                        base.push({ value: cur, label: cur })
                                    }
                                    return base
                                }
                                currentIndex: {
                                    const current = modelData.connection
                                    for (let i = 0; i < count; i++) {
                                        if (model[i] && model[i].value === current) return i
                                    }
                                    return 0
                                }
                                onActivated: function(idx) {
                                    const entry = model[idx]
                                    if (!entry) return
                                    const updated = purposes.slice()
                                    updated[index] = Object.assign({}, modelData, { connection: entry.value })
                                    // When the connection changes, drop a
                                    // model that no longer exists under it;
                                    // pick the first capability-matching
                                    // model on the new connection.
                                    const modelsForConn = modelsByConnection[entry.value] || []
                                    const wantsEmbedding = modelData.key === "embedding"
                                    const still = modelsForConn.find(function(m) {
                                        return m.id === modelData.model
                                    })
                                    if (!still) {
                                        const fallback = modelsForConn.find(function(m) {
                                            return Boolean(m.embedding) === wantsEmbedding
                                        })
                                        updated[index].model = fallback ? fallback.id : ""
                                    }
                                    purposes = updated
                                    persist(index)
                                }
                            }

                            QQC2.ComboBox {
                                Layout.fillWidth: true
                                textRole: "label"
                                model: {
                                    const base = []
                                    const sourceConn = modelData.connection
                                    if (sourceConn) {
                                        const models = modelsByConnection[sourceConn] || []
                                        // The embedding purpose only accepts
                                        // embedding-capable models; every
                                        // other purpose only accepts chat
                                        // (non-embedding) models.
                                        const wantsEmbedding = modelData.key === "embedding"
                                        for (let i = 0; i < models.length; i++) {
                                            const m = models[i]
                                            if (Boolean(m.embedding) !== wantsEmbedding) continue
                                            base.push({ value: m.id, label: m.display_name })
                                        }
                                    }
                                    // Always include the saved value so the
                                    // dropdown reflects the daemon state even
                                    // before list_available_models completes
                                    // (or when the connection can't enumerate
                                    // models at all, e.g. Bedrock without
                                    // network).
                                    const cur = modelData.model
                                    if (cur
                                        && !base.some(function(m) { return m.value === cur })) {
                                        base.push({ value: cur, label: cur })
                                    }
                                    return base
                                }
                                currentIndex: {
                                    const current = modelData.model
                                    for (let i = 0; i < count; i++) {
                                        if (model[i] && model[i].value === current) return i
                                    }
                                    return 0
                                }
                                onActivated: function(idx) {
                                    const entry = model[idx]
                                    if (!entry) return
                                    const updated = purposes.slice()
                                    updated[index] = Object.assign({}, modelData, { model: entry.value })
                                    purposes = updated
                                    persist(index)
                                }
                            }

                            QQC2.ComboBox {
                                Layout.preferredWidth: 140
                                textRole: "label"
                                model: [
                                    { value: "", label: "Effort: None" },
                                    { value: "low", label: "Effort: Low" },
                                    { value: "medium", label: "Effort: Medium" },
                                    { value: "high", label: "Effort: High" },
                                ]
                                currentIndex: {
                                    const current = modelData.effort || ""
                                    for (let i = 0; i < count; i++) {
                                        if (model[i] && model[i].value === current) return i
                                    }
                                    return 0
                                }
                                onActivated: function(idx) {
                                    const entry = model[idx]
                                    if (!entry) return
                                    const updated = purposes.slice()
                                    updated[index] = Object.assign({}, modelData, { effort: entry.value })
                                    purposes = updated
                                    persist(index)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        QQC2.Button {
            text: "Reload"
            icon.name: "view-refresh"
            enabled: !loadingPurposes && !loadingConnections
            onClicked: reload()
        }
    }
}
