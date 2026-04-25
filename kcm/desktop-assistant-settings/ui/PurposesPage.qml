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
            const out = []
            for (let i = 0; i < purposeOrder.length; i++) {
                const entry = purposeOrder[i]
                const cfg = view[entry.key] || {}
                out.push({
                    key: entry.key,
                    label: entry.label,
                    connection: String(cfg.connection || ""),
                    model: String(cfg.model || ""),
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
                byConn[cid].push({
                    id: String(modelObj.id || ""),
                    display_name: String(modelObj.display_name || modelObj.id || ""),
                })
            }
            modelsByConnection = byConn
        })
    }

    function persist(index) {
        const item = purposes[index]
        if (!item) return
        const config = {
            connection: item.connection || "primary",
            model: item.model || "primary",
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
        text: "Route each assistant purpose through a specific connection and model. Non-interactive purposes (titling, dreaming, embedding) can pick \"Same as Interactive Chat\" to follow whatever the interactive purpose is set to."
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
                                    // Non-interactive purposes can inherit
                                    // the primary (interactive) selection.
                                    // The wire value stays "primary" — the
                                    // daemon resolves inheritance — but the
                                    // label avoids implying "Primary" is a
                                    // connector type.
                                    if (modelData.key !== "interactive") {
                                        base.push({ value: "primary", label: "Same as Interactive Chat" })
                                    }
                                    for (let i = 0; i < connections.length; i++) {
                                        base.push({ value: connections[i].id, label: connections[i].label })
                                    }
                                    // Always include the saved value so the
                                    // dropdown reflects the daemon state even
                                    // before list_connections completes.
                                    const cur = modelData.connection
                                    if (cur && cur !== "primary"
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
                                    // When the connection changes, reset
                                    // model to "primary" (inherit) to avoid
                                    // dangling model ids on a new connector.
                                    if (entry.value === "primary") {
                                        updated[index].model = "primary"
                                    } else {
                                        const modelsForConn = modelsByConnection[entry.value] || []
                                        const still = modelsForConn.find(function(m) {
                                            return m.id === modelData.model
                                        })
                                        if (!still) {
                                            updated[index].model = modelsForConn.length > 0 ? modelsForConn[0].id : "primary"
                                        }
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
                                    const isInheritingConn = modelData.connection === "primary"
                                    base.push({
                                        value: "primary",
                                        label: isInheritingConn ? "Same as Interactive Chat" : "Same model as Interactive Chat"
                                    })
                                    // Enumerate models from this purpose's
                                    // connection — or, when inheriting, from
                                    // the interactive purpose's connection so
                                    // the user can override only the model.
                                    let sourceConn = modelData.connection
                                    if (isInheritingConn) {
                                        const interactivePurpose = purposes.find(function(p) { return p.key === "interactive" })
                                        sourceConn = interactivePurpose ? interactivePurpose.connection : ""
                                    }
                                    if (sourceConn && sourceConn !== "primary") {
                                        const models = modelsByConnection[sourceConn] || []
                                        for (let i = 0; i < models.length; i++) {
                                            base.push({ value: models[i].id, label: models[i].display_name })
                                        }
                                    }
                                    // Always include the saved value so the
                                    // dropdown reflects the daemon state even
                                    // before list_available_models completes
                                    // (or when the connection can't enumerate
                                    // models at all, e.g. Bedrock without
                                    // network).
                                    const cur = modelData.model
                                    if (cur && cur !== "primary"
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
