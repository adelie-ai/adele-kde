/*
 * Knowledge base browser/editor (#74). Mirrors the GTK app's KB widget
 * but lives inside the KCM as another tab. Talks to the daemon's
 * Knowledge interface (`org.desktopAssistant.Knowledge`) via the same
 * `kcm.daemonCall` JS bridge used by the Connections / Purposes pages.
 *
 * Layout: SearchEntry + ListView on the left, edit pane on the right
 * (content TextArea, comma-separated tags, JSON metadata). Save creates
 * or updates depending on whether an entry is selected; Delete confirms
 * via a small dialog before issuing the call.
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root
    spacing: 8

    // --- Page state ------------------------------------------------------

    property string statusText: ""
    property bool loading: false
    property var entries: []
    property string lastQuery: ""

    // Editor state. `selectedId === ""` means new-entry mode.
    property string selectedId: ""
    property string editorCreatedAt: ""
    property string editorUpdatedAt: ""

    // Search debounce: SearchField doesn't ship one out of the box, so
    // we run the query 250 ms after the last keystroke.
    Timer {
        id: searchTimer
        interval: 250
        repeat: false
        onTriggered: refresh()
    }

    function reload() {
        searchTimer.stop()
        refresh()
    }

    function refresh() {
        loading = true
        statusText = root.lastQuery.length > 0 ? "Searching…" : "Loading…"
        const cmd = root.lastQuery.length > 0 ? "search_knowledge_entries" : "list_knowledge_entries"
        const payload = root.lastQuery.length > 0
            ? { query: root.lastQuery, limit: 50 }
            : { limit: 100, offset: 0 }
        kcm.daemonCall(cmd, payload, function(result, error) {
            loading = false
            if (error) {
                statusText = error
                return
            }
            const rows = (result && result.knowledge_entries) ? result.knowledge_entries : []
            const slim = []
            for (let i = 0; i < rows.length; i++) {
                const r = rows[i] || {}
                slim.push({
                    id: String(r.id || ""),
                    content: String(r.content || ""),
                    tags: r.tags || [],
                    metadata: r.metadata || {},
                    created_at: String(r.created_at || ""),
                    updated_at: String(r.updated_at || ""),
                })
            }
            root.entries = slim
            statusText = formatStatus(slim.length, root.lastQuery)
        })
    }

    function formatStatus(count, query) {
        if (count === 0) {
            return query.length > 0 ? "No matches." : "No entries yet."
        }
        const noun = count === 1 ? "entry" : "entries"
        return query.length > 0
            ? (count + " " + noun + " match \"" + query + "\"")
            : (count + " " + noun)
    }

    function firstLine(text) {
        if (!text) return "(empty)"
        const idx = text.indexOf("\n")
        const line = (idx >= 0 ? text.substring(0, idx) : text).trim()
        return line.length > 0 ? line : "(empty)"
    }

    function selectEntry(entry) {
        root.selectedId = entry.id
        root.editorCreatedAt = entry.created_at
        root.editorUpdatedAt = entry.updated_at
        contentArea.text = entry.content
        tagsField.text = (entry.tags || []).join(", ")
        try {
            metadataArea.text = JSON.stringify(entry.metadata || {}, null, 2)
        } catch (e) {
            metadataArea.text = "{}"
        }
        statusText = ""
    }

    function clearEditor() {
        root.selectedId = ""
        root.editorCreatedAt = ""
        root.editorUpdatedAt = ""
        contentArea.text = ""
        tagsField.text = ""
        metadataArea.text = "{}"
        statusText = ""
    }

    function parseTags(raw) {
        if (!raw) return []
        const parts = raw.split(",")
        const out = []
        for (let i = 0; i < parts.length; i++) {
            const t = parts[i].trim()
            if (t.length > 0) out.push(t)
        }
        return out
    }

    function parseMetadata(raw) {
        const trimmed = (raw || "").trim()
        if (trimmed.length === 0) return {}
        return JSON.parse(trimmed)
    }

    function save() {
        const content = contentArea.text.trim()
        if (content.length === 0) {
            statusText = "Content is empty — nothing to save."
            return
        }
        const tags = parseTags(tagsField.text)
        let metadata
        try {
            metadata = parseMetadata(metadataArea.text)
        } catch (e) {
            statusText = "Invalid metadata JSON: " + e
            return
        }
        statusText = "Saving…"
        const cmd = root.selectedId.length > 0
            ? "update_knowledge_entry"
            : "create_knowledge_entry"
        const payload = root.selectedId.length > 0
            ? { id: root.selectedId, content: contentArea.text, tags: tags, metadata: metadata }
            : { content: contentArea.text, tags: tags, metadata: metadata }
        kcm.daemonCall(cmd, payload, function(result, error) {
            if (error) {
                statusText = "Save failed: " + error
                return
            }
            const saved = result && result.knowledge_entry_written
                ? result.knowledge_entry_written
                : null
            if (saved) {
                root.selectEntry({
                    id: String(saved.id || ""),
                    content: String(saved.content || ""),
                    tags: saved.tags || [],
                    metadata: saved.metadata || {},
                    created_at: String(saved.created_at || ""),
                    updated_at: String(saved.updated_at || ""),
                })
            }
            statusText = "Saved."
            refresh()
        })
    }

    function deleteSelected() {
        if (root.selectedId.length === 0) return
        const id = root.selectedId
        statusText = "Deleting…"
        kcm.daemonCall("delete_knowledge_entry", { id: id }, function(result, error) {
            if (error) {
                statusText = "Delete failed: " + error
                return
            }
            statusText = "Deleted."
            clearEditor()
            refresh()
        })
    }

    Component.onCompleted: refresh()

    // --- Layout ----------------------------------------------------------

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: statusText.length > 0
        text: statusText
        type: Kirigami.MessageType.Information
    }

    // Resizable split: drag the handle to widen the list or the editor. The
    // SplitView gives both panes a definite height, so their inner ScrollViews
    // actually scroll (instead of relying on a now-removed outer ScrollView).
    QQC2.SplitView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        orientation: Qt.Horizontal

        // Wider, discoverable drag handle — the default is a ~1px line that's
        // easy to miss. 8px hit area, an always-visible centre grip, and a
        // highlight tint on hover/drag.
        handle: Item {
            implicitWidth: 8
            implicitHeight: 8

            Kirigami.Separator {
                anchors.horizontalCenter: parent.horizontalCenter
                height: parent.height
            }

            Rectangle {
                anchors.fill: parent
                color: Kirigami.Theme.highlightColor
                opacity: QQC2.SplitHandle.pressed ? 0.4
                       : (QQC2.SplitHandle.hovered ? 0.2 : 0)
            }
        }

        // -- Left: search + list (resizable pane, scrollable list) --------
        ColumnLayout {
            QQC2.SplitView.preferredWidth: 320
            QQC2.SplitView.minimumWidth: 200
            spacing: 6

            Kirigami.SearchField {
                id: searchField
                Layout.fillWidth: true
                placeholderText: "Search entries…"
                onTextChanged: {
                    root.lastQuery = text
                    searchTimer.restart()
                }
            }

            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 160
                clip: true

                ListView {
                    id: listView
                    model: root.entries
                    spacing: 2
                    delegate: QQC2.ItemDelegate {
                        width: ListView.view ? ListView.view.width : 0
                        highlighted: root.selectedId === modelData.id
                        contentItem: ColumnLayout {
                            spacing: 2
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: root.firstLine(modelData.content)
                                elide: Text.ElideRight
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                visible: modelData.tags && modelData.tags.length > 0
                                opacity: 0.7
                                font.pointSize: Math.max(8, Kirigami.Theme.smallFont.pointSize)
                                text: (modelData.tags || []).join(", ")
                                elide: Text.ElideRight
                            }
                        }
                        onClicked: root.selectEntry(modelData)
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true

                QQC2.Label {
                    Layout.fillWidth: true
                    text: root.loading ? "Loading…" : (root.entries.length + " loaded")
                    opacity: 0.6
                }

                QQC2.Button {
                    text: "Refresh"
                    icon.name: "view-refresh"
                    enabled: !root.loading
                    onClicked: root.reload()
                }

                QQC2.Button {
                    text: "+ New"
                    icon.name: "list-add"
                    onClicked: root.clearEditor()
                }
            }
        }

        // -- Right: editor (scrollable content) ---------------------------
        ColumnLayout {
            QQC2.SplitView.fillWidth: true
            QQC2.SplitView.minimumWidth: 280
            spacing: 6

            QQC2.Label {
                font.bold: true
                text: root.selectedId.length > 0 ? root.selectedId : "New entry"
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            QQC2.Label {
                Layout.fillWidth: true
                visible: root.editorUpdatedAt.length > 0
                opacity: 0.6
                elide: Text.ElideRight
                text: "Updated " + root.editorUpdatedAt + " · created " + root.editorCreatedAt
            }

            // Vertical split inside the editor: drag the divider to rebalance
            // the Content box against Tags + Metadata. Defaults to ~60/40 so
            // the lower fields aren't squished into a sliver. Each pane scrolls
            // internally, so a long Content box stays scrollable rather than
            // growing without bound.
            QQC2.SplitView {
                id: editorSplit
                Layout.fillWidth: true
                Layout.fillHeight: true
                orientation: Qt.Vertical

                handle: Item {
                    implicitWidth: 8
                    implicitHeight: 8

                    Kirigami.Separator {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                    }

                    Rectangle {
                        anchors.fill: parent
                        color: Kirigami.Theme.highlightColor
                        opacity: QQC2.SplitHandle.pressed ? 0.4
                               : (QQC2.SplitHandle.hovered ? 0.2 : 0)
                    }
                }

                // -- Content (top, ~60%) --
                ColumnLayout {
                    QQC2.SplitView.fillHeight: true
                    QQC2.SplitView.minimumHeight: 96
                    spacing: 6

                    QQC2.Label { text: "Content" }

                    QQC2.ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        QQC2.TextArea {
                            id: contentArea
                            wrapMode: TextEdit.Wrap
                            placeholderText: "Free-form prose. The daemon chunks + embeds this on save."
                        }
                    }
                }

                // -- Tags + Metadata (bottom, ~40%) --
                ColumnLayout {
                    QQC2.SplitView.preferredHeight: Math.round(editorSplit.height * 0.4)
                    QQC2.SplitView.minimumHeight: 96
                    spacing: 6

                    QQC2.Label { text: "Tags (comma-separated)" }

                    QQC2.TextField {
                        id: tagsField
                        Layout.fillWidth: true
                        placeholderText: "preference, project:foo, instruction"
                    }

                    QQC2.Label { text: "Metadata (JSON)" }

                    QQC2.ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        QQC2.TextArea {
                            id: metadataArea
                            wrapMode: TextEdit.Wrap
                            font.family: "monospace"
                            text: "{}"
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignRight

                Item { Layout.fillWidth: true }

                QQC2.Button {
                    text: "Delete"
                    icon.name: "edit-delete"
                    enabled: root.selectedId.length > 0
                    onClicked: confirmDeleteDialog.open()
                }

                QQC2.Button {
                    text: "Save"
                    icon.name: "document-save"
                    onClicked: root.save()
                }
            }
        }
    }

    // --- Confirm-delete dialog ------------------------------------------

    QQC2.Dialog {
        id: confirmDeleteDialog
        title: "Delete entry?"
        modal: true
        anchors.centerIn: parent
        standardButtons: QQC2.Dialog.Cancel | QQC2.Dialog.Ok

        QQC2.Label {
            text: "Delete this entry? This cannot be undone."
            wrapMode: Text.WordWrap
        }

        onAccepted: root.deleteSelected()
    }
}
