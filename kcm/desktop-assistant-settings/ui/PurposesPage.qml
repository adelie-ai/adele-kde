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
    property int persistCalls: 0
    property string persistLast: "(no persist call yet)"

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
        persistCalls += 1
        const item = purposes[index]
        if (!item) {
            persistLast = "#" + persistCalls + " no item at index " + index
            return
        }
        const config = {
            connection: item.connection || "",
            model: item.model || "",
        }
        if (item.effort && item.effort.length > 0) {
            config.effort = item.effort
        }
        persistLast = "#" + persistCalls + " " + item.key + " " + JSON.stringify(config)
        kcm.wsCall("set_purpose", { purpose: item.key, config: config }, function(_result, error) {
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
        font.family: "monospace"
        font.italic: true
        opacity: 0.7
        wrapMode: Text.Wrap
        text: "persist calls: " + persistCalls + " — last: " + persistLast
    }

    // Plain top-level ComboBox to confirm whether ComboBox.activated
    // works at all in this page (independent of the Repeater delegate
    // context where the production ComboBoxes live).
    RowLayout {
        Layout.fillWidth: true
        QQC2.Label { text: "TEST ComboBox:" }
        QQC2.ComboBox {
            id: testCombo
            Layout.fillWidth: true
            model: ["alpha", "beta", "gamma"]
            onActivated: {
                persistCalls += 1
                persistLast = "[TEST inline-onActivated] idx=" + index + " value=" + testCombo.model[index]
            }
        }
        Connections {
            target: testCombo
            function onActivated(idx) {
                persistCalls += 1
                persistLast = "[TEST Connections.onActivated] idx=" + idx + " value=" + testCombo.model[idx]
            }
        }
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

                        QQC2.Label {
                            text: purposeCard.rowData.label
                            font.bold: true
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            QQC2.ComboBox {
                                id: connectionBox
                                Layout.fillWidth: true
                                textRole: "label"
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
                                Component.onCompleted: rebuild()
                                Connections {
                                    target: root
                                    function onConnectionsChanged() { connectionBox.rebuild() }
                                }
                                Connections {
                                    target: purposeCard
                                    function onRowDataChanged() { connectionBox.rebuild() }
                                }
                                Connections {
                                    target: connectionBox
                                    function onActivated(idx) {
                                        const entry = connectionBox.model[idx]
                                        if (!entry) return
                                        const updated = purposes.slice()
                                        updated[purposeCard.rowIndex] = Object.assign({}, purposeCard.rowData, { connection: entry.value })
                                        const modelsForConn = modelsByConnection[entry.value] || []
                                        const wantsEmbedding = purposeCard.rowData.key === "embedding"
                                        const still = modelsForConn.find(function(m) {
                                            return m.id === purposeCard.rowData.model
                                        })
                                        if (!still) {
                                            const fallback = modelsForConn.find(function(m) {
                                                return Boolean(m.embedding) === wantsEmbedding
                                            })
                                            if (fallback) {
                                                updated[purposeCard.rowIndex].model = fallback.id
                                            }
                                        }
                                        purposes = updated
                                        persist(purposeCard.rowIndex)
                                    }
                                }
                            }

                            QQC2.ComboBox {
                                id: modelBox
                                Layout.fillWidth: true
                                textRole: "label"
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
                                Component.onCompleted: rebuild()
                                Connections {
                                    target: root
                                    function onModelsByConnectionChanged() { modelBox.rebuild() }
                                }
                                Connections {
                                    target: purposeCard
                                    function onRowDataChanged() { modelBox.rebuild() }
                                }
                                Connections {
                                    target: modelBox
                                    function onActivated(idx) {
                                        const entry = modelBox.model[idx]
                                        if (!entry) return
                                        const updated = purposes.slice()
                                        updated[purposeCard.rowIndex] = Object.assign({}, purposeCard.rowData, { model: entry.value })
                                        purposes = updated
                                        persist(purposeCard.rowIndex)
                                    }
                                }
                            }

                            QQC2.ComboBox {
                                id: effortBox
                                Layout.preferredWidth: 140
                                textRole: "label"
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
                                Component.onCompleted: syncIndex()
                                Connections {
                                    target: purposeCard
                                    function onRowDataChanged() { effortBox.syncIndex() }
                                }
                                Connections {
                                    target: effortBox
                                    function onActivated(idx) {
                                        const entry = effortBox.model[idx]
                                        if (!entry) return
                                        const updated = purposes.slice()
                                        updated[purposeCard.rowIndex] = Object.assign({}, purposeCard.rowData, { effort: entry.value })
                                        purposes = updated
                                        persist(purposeCard.rowIndex)
                                    }
                                }
                            }

                            // Sibling test ComboBox inside the same delegate
                            // to prove whether ComboBox.activated fires
                            // *inside* the Repeater delegate context. If the
                            // page-level test combo fires but this one
                            // doesn't, the bug is the delegate context.
                            QQC2.ComboBox {
                                id: rowTestCombo
                                Layout.preferredWidth: 120
                                model: ["test-a", "test-b", "test-c"]
                                onActivated: {
                                    persistCalls += 1
                                    persistLast = "[ROW-TEST inline " + purposeCard.rowData.key + "] idx=" + index
                                }
                                Connections {
                                    target: rowTestCombo
                                    function onActivated(idx) {
                                        persistCalls += 1
                                        persistLast = "[ROW-TEST Connections " + purposeCard.rowData.key + "] idx=" + idx
                                    }
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
