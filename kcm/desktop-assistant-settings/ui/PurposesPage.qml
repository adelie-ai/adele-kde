/*
 * Purposes page (issue adele-kde#1).
 *
 * Shows one row per purpose — interactive / dreaming (extraction) /
 * consolidation / titling / embedding — and lets the user bind each to a
 * `(connection, model, effort)` tuple via
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
    // the response object directly. Each entry carries a short `description`
    // surfaced via a hover info-icon, keeping the row itself terse.
    readonly property var purposeOrder: [
        {
            key: "interactive",
            label: "Interactive (chat)",
            description: "The model that powers live conversation with you."
        },
        {
            key: "dreaming",
            label: "Dreaming: Extraction (quick/cheap)",
            description: "Frequent, lightweight pass that pulls durable facts out of each "
                + "conversation into the knowledge base. A small or local model is fine here."
        },
        {
            key: "consolidation",
            label: "Dreaming: Consolidation (slower, bigger model)",
            description: "Slower daily pass that reviews the whole knowledge base to merge "
                + "duplicates, tighten entries, and prune low-value notes. Benefits from a "
                + "stronger model."
        },
        {
            key: "titling",
            label: "Titling (conversation names)",
            description: "Generates short titles for conversations."
        },
        {
            key: "embedding",
            label: "Embedding (semantic search)",
            description: "Produces the vectors used for knowledge-base semantic search."
        },
    ]

    function reload() {
        loadingPurposes = true
        loadingConnections = true

        kcm.daemonCall("list_connections", {}, function(result, error) {
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

        kcm.daemonCall("get_purposes", {}, function(result, error) {
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
                    description: entry.description || "",
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
        kcm.daemonCall("list_available_models", {}, function(result, error) {
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

    // `item` is the snapshot to persist — pass it directly rather than
    // looking it up from `purposes[index]`. The lookup approach broke when
    // the caller had to mutate `purposes` first to populate the entry, and
    // mutating `purposes` triggers Repeater delegate rebuilds that kill
    // the calling JS context before persist() got to run.
    function persist(item) {
        if (!item) return
        const config = {
            connection: item.connection || "",
            model: item.model || "",
        }
        if (item.effort && item.effort.length > 0) {
            config.effort = item.effort
        }
        kcm.daemonCall("set_purpose", { purpose: item.key, config: config }, function(_result, error) {
            if (error) {
                statusText = "Failed to save purpose '" + item.key + "': " + error
                return
            }
            statusText = ""
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
        type: Kirigami.MessageType.Warning
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
                    id: purposeCard
                    Layout.fillWidth: true
                    // Capture the Repeater's row index into a named property
                    // so signal handlers below can reference it without
                    // colliding with the ComboBox.activated signal's own
                    // `index` parameter.
                    property int rowIndex: index
                    property var rowData: modelData

                    contentItem: ColumnLayout {
                        spacing: 4

                        RowLayout {
                            spacing: Kirigami.Units.smallSpacing

                            QQC2.Label {
                                text: purposeCard.rowData.label
                                font.bold: true
                            }

                            // Exposition lives behind a hover info-icon rather
                            // than inline text, keeping each row terse.
                            InfoTip {
                                text: purposeCard.rowData.description || ""
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            QQC2.ComboBox {
                                id: connectionBox
                                Layout.fillWidth: true
                                textRole: "label"
                                valueRole: "value"
                                // model and currentIndex are both imperative.
                                // A `model: { ... }` JS-expression binding
                                // recomputes between currentIndexChanged and
                                // activated when its deps tick, and Qt drops
                                // activated as a result. A `currentIndex:`
                                // binding similarly fights user picks. Keep
                                // both explicitly under our control.
                                property var items: []
                                model: items
                                function rebuild() {
                                    const base = []
                                    for (let i = 0; i < connections.length; i++) {
                                        base.push({ value: connections[i].id, label: connections[i].label })
                                    }
                                    const cur = purposeCard.rowData.connection
                                    if (cur
                                        && !base.some(function(m) { return m.value === cur })) {
                                        base.push({ value: cur, label: cur })
                                    }
                                    items = base
                                    syncIndex()
                                }
                                function syncIndex() {
                                    const current = purposeCard.rowData.connection
                                    for (let i = 0; i < items.length; i++) {
                                        if (items[i].value === current) {
                                            currentIndex = i
                                            return
                                        }
                                    }
                                    currentIndex = 0
                                }
                                Component.onCompleted: {
                                    rebuild()
                                    connectionBox.activated.connect(function(idx) {
                                        const value = connectionBox.currentValue
                                        if (!value) return
                                        const newItem = Object.assign({}, purposeCard.rowData, { connection: value })
                                        const modelsForConn = modelsByConnection[value] || []
                                        const wantsEmbedding = purposeCard.rowData.key === "embedding"
                                        const still = modelsForConn.find(function(m) { return m.id === purposeCard.rowData.model })
                                        if (!still) {
                                            const fallback = modelsForConn.find(function(m) { return Boolean(m.embedding) === wantsEmbedding })
                                            if (fallback) newItem.model = fallback.id
                                        }
                                        const rowIdx = purposeCard.rowIndex
                                        persist(newItem)
                                        const updated = purposes.slice()
                                        updated[rowIdx] = newItem
                                        purposes = updated
                                    })
                                }
                                Connections {
                                    target: root
                                    function onConnectionsChanged() { connectionBox.rebuild() }
                                }
                                Connections {
                                    target: purposeCard
                                    function onRowDataChanged() { connectionBox.rebuild() }
                                }
                            }

                            QQC2.ComboBox {
                                id: modelBox
                                Layout.fillWidth: true
                                textRole: "label"
                                valueRole: "value"
                                property var items: []
                                model: items
                                function rebuild() {
                                    const base = []
                                    const sourceConn = purposeCard.rowData.connection
                                    if (sourceConn) {
                                        const models = modelsByConnection[sourceConn] || []
                                        // The embedding purpose only accepts
                                        // embedding-capable models; every
                                        // other purpose only accepts chat
                                        // (non-embedding) models.
                                        const wantsEmbedding = purposeCard.rowData.key === "embedding"
                                        for (let i = 0; i < models.length; i++) {
                                            const m = models[i]
                                            if (Boolean(m.embedding) !== wantsEmbedding) continue
                                            base.push({ value: m.id, label: m.display_name })
                                        }
                                    }
                                    const cur = purposeCard.rowData.model
                                    if (cur
                                        && !base.some(function(m) { return m.value === cur })) {
                                        base.push({ value: cur, label: cur })
                                    }
                                    items = base
                                    syncIndex()
                                }
                                function syncIndex() {
                                    const current = purposeCard.rowData.model
                                    for (let i = 0; i < items.length; i++) {
                                        if (items[i].value === current) {
                                            currentIndex = i
                                            return
                                        }
                                    }
                                    currentIndex = 0
                                }
                                Component.onCompleted: {
                                    rebuild()
                                    // Imperative connect — bypasses every
                                    // declarative form that the QML
                                    // compiler kept dropping in this
                                    // delegate context. Use the ComboBox's
                                    // built-in currentValue rather than
                                    // peeking into items[idx], which was
                                    // returning undefined for reasons that
                                    // are not worth the time to chase.
                                    modelBox.activated.connect(function(idx) {
                                        const value = modelBox.currentValue
                                        if (!value) return
                                        // Build the new snapshot, persist it,
                                        // THEN mutate `purposes`. Mutating
                                        // first kicks off Repeater delegate
                                        // rebuilds and kills this JS context
                                        // before persist() ran.
                                        const newItem = Object.assign({}, purposeCard.rowData, { model: value })
                                        const rowIdx = purposeCard.rowIndex
                                        persist(newItem)
                                        const updated = purposes.slice()
                                        updated[rowIdx] = newItem
                                        purposes = updated
                                    })
                                }
                                Connections {
                                    target: root
                                    function onModelsByConnectionChanged() { modelBox.rebuild() }
                                }
                                Connections {
                                    target: purposeCard
                                    function onRowDataChanged() { modelBox.rebuild() }
                                }
                            }

                            QQC2.ComboBox {
                                id: effortBox
                                Layout.preferredWidth: 140
                                textRole: "label"
                                valueRole: "value"
                                model: [
                                    { value: "", label: "Effort: None" },
                                    { value: "low", label: "Effort: Low" },
                                    { value: "medium", label: "Effort: Medium" },
                                    { value: "high", label: "Effort: High" },
                                ]
                                function syncIndex() {
                                    const current = purposeCard.rowData.effort || ""
                                    for (let i = 0; i < count; i++) {
                                        if (model[i] && model[i].value === current) {
                                            currentIndex = i
                                            return
                                        }
                                    }
                                    currentIndex = 0
                                }
                                Component.onCompleted: {
                                    syncIndex()
                                    effortBox.activated.connect(function(idx) {
                                        const value = effortBox.currentValue || ""
                                        const newItem = Object.assign({}, purposeCard.rowData, { effort: value })
                                        const rowIdx = purposeCard.rowIndex
                                        persist(newItem)
                                        const updated = purposes.slice()
                                        updated[rowIdx] = newItem
                                        purposes = updated
                                    })
                                }
                                Connections {
                                    target: purposeCard
                                    function onRowDataChanged() { effortBox.syncIndex() }
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
