// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The KDE chat view. All model/controller/transport logic lives in the shared
// Rust core (org.desktopassistant.client): `AdeleCore` owns the client-ui-common
// reducer + a client-common Connector in D-Bus mode (the org.desktopAssistant
// bridge), and `VoiceController` is native QtDBus glue for the separate voice
// daemon (org.desktopAssistant.Voice). This QML is a thin VIEW: it renders the
// deltas the core pushes via `viewEvent(type, data)` and forwards user actions as
// intents. There is no polling, no daemon parsing, and no Python helper — the
// reducer already decided what changed, so there is no controller logic here.
//
// Live cross-client sync (#367), streaming, the conversation list, and (now, over
// D-Bus for the first time) the initial loads all flow through the core's signal
// pump. See the worktree's client/ plugin and client-ui-common/ffi.

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtCore
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid

import org.desktopassistant.client

import "LinkSafety.js" as LinkSafety
import "QueueRecall.js" as QueueRecall

Item {
    id: root
    clip: true
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    property bool panelMode: false
    implicitWidth: 520
    implicitHeight: 620

    readonly property color themeBackgroundColor: Kirigami.Theme.backgroundColor
    readonly property color themeTextColor: Kirigami.Theme.textColor
    readonly property color themeDisabledTextColor: Kirigami.Theme.disabledTextColor
    readonly property color themeHighlightColor: Kirigami.Theme.highlightColor
    readonly property color themeHighlightedTextColor: Kirigami.Theme.highlightedTextColor

    // --- main.qml contract -------------------------------------------------
    // The plasmoid shell reads these off the loaded item: a hide flag, the voice
    // wake-word state for the "Enable Hey Adele" context action, and a tasks
    // backend + badge-click signal for the process-manager window. Voice state is
    // forwarded straight from the VoiceController so the shell needs no change.
    readonly property bool hideWidget: false
    property alias voiceAvailable: voice.available
    property alias voiceEnabled: voice.enabled
    function setVoiceEnabled(enabled) {
        voice.setEnabled(enabled)
    }
    signal tasksBadgeClicked()
    // Background-tasks backend for the header badge + the process-manager window
    // (the Tasks*.qml read this interface). Driven by the core's Task* events;
    // the getters read reactive root properties so the views rebind on change.
    readonly property var tasksBackend: ({
        get tasks() { return root.tasksList },
        get runningTaskCount() { return root.runningTaskCount },
        cancelTask: function(id) { core.cancelTask(String(id || "")) },
        openConversation: function(id) { root.openTaskConversation(String(id || "")) },
        refreshLogs: function(id) { core.fetchTaskLogs(String(id || "")) },
        taskLogs: function(id) { return root.taskLogsById[String(id || "")] || "" }
    })

    // --- view state (driven by the core's viewEvent deltas) ----------------
    property string conversationId: ""
    property string connectionLabel: ""
    // The reducer disables sending while a reply streams; we mirror it as `busy`
    // to gate the composer + flip the header avatar to "thinking".
    property bool busy: false
    property string statusText: "Connecting…"
    // Transient "Thinking…" line under the transcript while a turn is in flight.
    property string chatStatusText: ""
    // Sidebar list: [{id, title, message_count, archived}].
    property var conversationChoices: []
    // Index of the in-progress assistant bubble in transcriptModel, or -1.
    property int streamingIndex: -1
    // Context-window fill (#76): the `context_usage` event payload (or null).
    property var contextUsage: null
    // Per-conversation You/Adele voice settings (#80), kept in QML-local maps:
    // the QML is the source of user-driven changes (sent via the reducer's
    // setVoiceIn/setAdeleOutput intents), and the model-driven Adele level
    // arrives via the `adele_output_dropdown` event. Mutated with the QV4-safe
    // assign-then-key pattern (Object.assign to the GC root, then write the key)
    // to avoid the insertMember GC hazard.
    property var voiceInByConv: ({})
    property var adeleOutByConv: ({})
    readonly property bool voiceInEnabled: voiceInByConv[conversationId] === true
    readonly property string adeleOutputLevel: String(adeleOutByConv[conversationId] || "disabled")
    // Model picker: available (connection·model) listings, the conversation's
    // stored selection, the interactive-purpose default, and whether the picker
    // should show — all from the core's events. A pick is applied via the
    // selectModel intent (the override is staged in the core).
    property var modelChoices: []
    property var currentSelection: null
    property var defaultModel: null
    property bool modelPickerVisible: false
    // Background tasks: the list + running count (from the Task* events) and a
    // per-task formatted log buffer (fetched via fetchTaskLogs / appended live).
    property var tasksList: []
    property int runningTaskCount: 0
    property var taskLogsById: ({})
    // Message queue (submit while busy). The composer stays editable/submittable
    // while a reply streams; a submit is enqueued by the core, surfaced here as
    // removable/editable chips (queuedMessagesModel) plus the index currently
    // checked out for editing (-1 = none). Both are driven by the core's
    // `queued_messages` event; the composer text is driven by `composer_text`.
    property int editingQueuedIndex: -1

    // --- view configuration (preserved styling) ----------------------------
    property real uiScale: 1.0
    readonly property real minUiScale: 0.9
    readonly property real maxUiScale: 1.35
    readonly property real zoomStep: 0.05
    readonly property string adeleAvatarSource: Qt.resolvedUrl("../images/adele.png")
    property string configuredUserAvatarPath: String(Plasmoid.configuration.userAvatarPath || "").trim()
    readonly property real baseFontPointSize: Math.max(1, Number(Kirigami.Theme.defaultFont.pointSize || Qt.application.font.pointSize || 10))
    readonly property int scaledTopIconSize: Math.max(16, Math.round(24 * uiScale))
    readonly property int scaledHeaderIconSize: Math.max(64, Math.round(96 * uiScale))
    readonly property bool ultraNarrow: width > 0 && width < 430
    readonly property real transcriptAvatarSize: 24 * uiScale
    readonly property real transcriptBubbleSpacing: 6
    readonly property real transcriptWideBubbleWidth: Math.max(120, transcript.width)
    readonly property real transcriptMessageBubbleWidth: {
        const available = Math.max(120, transcript.width - (transcriptAvatarSize + transcriptBubbleSpacing))
        return Math.max(120, available * 0.88)
    }
    readonly property string homeDirectory: StandardPaths.writableLocation(StandardPaths.HomeLocation)
    readonly property string accountName: {
        const trimmedHome = String(homeDirectory || "").replace(/\/+$/, "")
        const chunks = trimmedHome.split("/").filter(function(chunk) { return chunk.length > 0 })
        return chunks.length > 0 ? chunks[chunks.length - 1] : ""
    }

    // --- voice state derived from the VoiceController -----------------------
    readonly property bool voiceActive: voice.available && voice.state !== "Idle"
    readonly property bool voiceListening: voice.available && voice.state === "Listening"
    readonly property bool voiceProcessing: voice.available && voice.state === "Processing"

    readonly property string voiceStateLabel: {
        if (!voice.available) return ""
        switch (voice.state) {
            case "Listening": return "Listening…"
            case "Processing": return "Thinking…"
            case "Speaking": return "Speaking…"
            default: return "Idle"
        }
    }
    readonly property string voiceStateIcon: {
        switch (voice.state) {
            case "Listening": return "audio-input-microphone"
            case "Processing": return "view-refresh-symbolic"
            case "Speaking": return "audio-volume-high"
            default: return "audio-input-microphone-muted"
        }
    }
    readonly property color voiceStateColor: {
        switch (voice.state) {
            case "Listening": return Kirigami.Theme.negativeTextColor
            case "Speaking": return themeHighlightColor
            case "Processing": return Kirigami.Theme.neutralTextColor
            default: return themeDisabledTextColor
        }
    }

    // ======================================================================
    //  The shared Rust core + the native voice glue
    // ======================================================================
    AdeleCore {
        id: core
        onViewEvent: function(type, data) { root.handleViewEvent(type, data) }
    }

    VoiceController {
        id: voice
    }

    // ======================================================================
    //  view-event handling (the reducer's deltas → render state)
    // ======================================================================
    function handleViewEvent(type, data) {
        switch (type) {
        case "connected":
            root.connectionLabel = String(data.label || "")
            root.statusText = ""
            break
        case "connect_error":
            root.statusText = "Connection failed: " + String(data.message || "")
            break
        case "client_cleared":
            root.statusText = "Disconnected"
            break
        case "status":
            root.statusText = String(data.text || "")
            break
        case "send_sensitive":
            root.busy = !(data.value === true)
            break
        case "conversations":
            root.conversationChoices = data.items || []
            root.syncConversationPicker()
            break
        case "load_conversation":
            root.loadConversation(data.detail || {})
            break
        case "clear_chat":
            transcriptModel.clear()
            root.streamingIndex = -1
            break
        case "chat_status":
            root.chatStatusText = String(data.text || "")
            break
        case "clear_chat_status":
            root.chatStatusText = ""
            break
        case "add_user_message":
            root.appendMessage("user", String(data.content || ""))
            break
        case "chunk":
            root.appendChunk(String(data.text || ""))
            break
        case "complete":
            root.completeStreaming(String(data.text || ""))
            break
        case "inline_note":
            root.appendNote(String(data.text || ""))
            break
        case "toast":
            root.statusText = String(data.text || "")
            break
        case "speak":
            // Route the model's say_this through the voice daemon's TTS.
            voice.sayText(String(data.text || ""))
            break
        case "composer_text":
            // The core sets the live composer: recall load, or an empty string
            // to clear on enqueue / cancel. Move the caret to the end.
            promptInput.text = String(data.text || "")
            promptInput.cursorPosition = promptInput.length
            break
        case "queued_messages":
            root.applyQueuedMessages(data.messages || [], data.editing)
            break
        case "context_usage":
            root.contextUsage = data.usage || null
            break
        case "adele_output_dropdown":
            // The model drove the Adele level (request_voice / stop_voice).
            root.applyAdeleLevelFromCore(String(data.level || "disabled"))
            break
        case "models":
            root.modelChoices = data.items || []
            root.syncModelPicker()
            break
        case "model_selection":
            root.currentSelection = data.selection || null
            root.syncModelPicker()
            break
        case "default_model":
            root.defaultModel = data.model || null
            root.syncModelPicker()
            break
        case "model_picker_visible":
            root.modelPickerVisible = data.value === true
            break
        case "tasks_replace_all":
            root.applyTasksReplaceAll(data.items || [])
            break
        case "task_started":
            root.applyTaskStarted(data.task || null)
            break
        case "task_progress":
            root.applyTaskProgress(String(data.id || ""), data.progress_hint)
            break
        case "task_log_appended":
            root.applyTaskLogAppended(String(data.id || ""), data.entry || null)
            break
        case "task_completed":
            root.applyTaskCompleted(String(data.id || ""))
            break
        case "task_logs":
            root.applyTaskLogs(String(data.id || ""), data.entries || [])
            break
        case "refresh_side_pane_tasks":
            // KDE has no per-conversation side pane; the badge + window already
            // reflect the full task list, so there's nothing to recompute.
            break
        // Ignored: scratchpad (no KDE side pane).
        default:
            break
        }
    }

    function loadConversation(detail) {
        root.conversationId = String(detail.id || "")
        transcriptModel.clear()
        root.streamingIndex = -1
        const messages = detail.messages || []
        for (let i = 0; i < messages.length; i++) {
            transcriptModel.append({
                kind: "message",
                role: String(messages[i].role || ""),
                body: String(messages[i].content || ""),
            })
        }
        root.syncConversationPicker()
        root.syncVoiceModeControls()
        Qt.callLater(function() {
            if (transcript && transcript.count > 0) {
                transcript.positionViewAtEnd()
            }
        })
    }

    // Rebuild the queued-message chips from the core's snapshot (submit order)
    // and record which index is checked out for editing (-1 = none). A ListModel
    // sidesteps the QV4 insertMember GC hazard a `property var` array carries.
    function applyQueuedMessages(messages, editing) {
        queuedMessagesModel.clear()
        for (let i = 0; i < messages.length; i++) {
            queuedMessagesModel.append({ text: String(messages[i]) })
        }
        root.editingQueuedIndex = QueueRecall.normalizeEditing(editing)
    }

    function appendMessage(role, body) {
        transcriptModel.append({ kind: "message", role: role, body: body })
        root.maybeStickToBottom()
    }

    function appendNote(text) {
        transcriptModel.append({ kind: "note", role: "assistant", body: text })
        root.maybeStickToBottom()
    }

    function appendChunk(text) {
        if (root.streamingIndex < 0) {
            transcriptModel.append({ kind: "message", role: "assistant", body: "" })
            root.streamingIndex = transcriptModel.count - 1
        }
        const current = transcriptModel.get(root.streamingIndex)
        transcriptModel.setProperty(root.streamingIndex, "body", (current ? current.body : "") + text)
        root.maybeStickToBottom()
    }

    function completeStreaming(text) {
        if (root.streamingIndex >= 0) {
            transcriptModel.setProperty(root.streamingIndex, "body", text)
            root.streamingIndex = -1
        } else {
            transcriptModel.append({ kind: "message", role: "assistant", body: text })
        }
        root.maybeStickToBottom()
    }

    // Stick to the newest message only when the user is already near the bottom,
    // so a streaming reply scrolls into view without yanking the viewport while
    // they're scrolled up reading history.
    function maybeStickToBottom() {
        if (!transcript) {
            return
        }
        const nearBottom = transcript.contentHeight <= transcript.height
            || (transcript.contentY + transcript.height) >= (transcript.contentHeight - 48)
        if (nearBottom) {
            Qt.callLater(function() {
                if (transcript && transcript.count > 0) {
                    transcript.positionViewAtEnd()
                }
            })
        }
    }

    // ======================================================================
    //  intents (user actions → the core)
    // ======================================================================
    function submitPrompt() {
        // No busy guard: the composer stays submittable while a reply streams.
        // The core sends when idle and enqueues while busy; either way it drives
        // the composer via `composer_text`. We also clear locally so the idle
        // path always empties the field even if no event follows.
        const text = promptInput.text
        if (text.trim().length === 0) {
            return
        }
        core.sendPrompt(text)
        promptInput.clear()
    }

    function conversationIndexById(id) {
        for (let i = 0; i < conversationChoices.length; i++) {
            if (conversationChoices[i].id === id) {
                return i
            }
        }
        return -1
    }

    function selectConversationById(id) {
        const target = String(id || "")
        if (target.length === 0) {
            return
        }
        core.selectConversation(target)
    }

    function syncConversationPicker() {
        const idx = conversationIndexById(conversationId)
        if (idx >= 0 && conversationPicker.currentIndex !== idx) {
            conversationPicker.currentIndex = idx
        }
    }

    // ======================================================================
    //  voice intent helpers (delegate to the VoiceController)
    // ======================================================================
    function voiceIndexById(id) {
        const list = voice.voices
        for (let i = 0; i < list.length; i++) {
            if (list[i].voice_id === id) {
                return i
            }
        }
        return -1
    }

    function voiceSpeakerCount(id) {
        const idx = voiceIndexById(id)
        return idx >= 0 ? Math.max(1, Number(voice.voices[idx].num_speakers || 1)) : 1
    }

    // The mic button is a toggle: start a dictation turn from Idle, otherwise
    // stop the active one (StopSpeaking while Speaking, else StopListening).
    function voiceMicToggle() {
        if (!voice.available) {
            return
        }
        if (voice.state === "Speaking") {
            voice.stopSpeaking()
        } else if (voice.state !== "Idle") {
            voice.stopListening()
        } else {
            voice.pushToTalk(root.conversationId)
        }
    }

    // The dedicated cancel affordance only ever STOPS the current turn.
    function voiceCancelTurn() {
        if (!root.voiceActive) {
            return
        }
        if (voice.state === "Speaking") {
            voice.stopSpeaking()
        } else {
            voice.stopListening()
        }
    }

    function micButtonTooltip() {
        if (voice.state === "Speaking") {
            return "Stop speaking"
        }
        if (voice.state !== "Idle") {
            return "Stop listening" + (voiceStateLabel.length > 0 ? " — " + voiceStateLabel : "")
        }
        return "Push to talk" + (voiceStateLabel.length > 0 ? " — " + voiceStateLabel : "")
    }

    // ======================================================================
    //  per-conversation voice mode (You / Adele, #80)
    // ======================================================================
    function setVoiceInForCurrent(enabled) {
        if (conversationId.length === 0) {
            return
        }
        // QV4-safe: assign the new object to the GC-rooted property first, then
        // write the key (never mutate-then-store).
        voiceInByConv = Object.assign({}, voiceInByConv)
        voiceInByConv[conversationId] = enabled
        core.setVoiceIn(conversationId, enabled)
    }
    function setAdeleOutputForCurrent(level) {
        if (conversationId.length === 0) {
            return
        }
        adeleOutByConv = Object.assign({}, adeleOutByConv)
        adeleOutByConv[conversationId] = level
        core.setAdeleOutput(conversationId, level)
    }
    // The model drove the Adele level (request_voice / stop_voice). Record it for
    // the active conversation and reflect it on the dropdown — but do NOT call
    // setAdeleOutput, which would echo the model's own change back to the core.
    function applyAdeleLevelFromCore(level) {
        if (conversationId.length === 0) {
            return
        }
        adeleOutByConv = Object.assign({}, adeleOutByConv)
        adeleOutByConv[conversationId] = level
        adeleCombo.currentIndex = adeleLevelToIndex(level)
    }
    function adeleLevelToIndex(level) {
        return level === "always" ? 2 : (level === "on_demand" ? 1 : 0)
    }
    function adeleIndexToLevel(index) {
        return index === 2 ? "always" : (index === 1 ? "on_demand" : "disabled")
    }
    // Reflect the active conversation's stored You/Adele settings on the
    // dropdowns. Called on load; currentIndex is set imperatively (not bound) so
    // a user pick doesn't break a declarative binding.
    function syncVoiceModeControls() {
        youCombo.currentIndex = voiceInEnabled ? 1 : 0
        adeleCombo.currentIndex = adeleLevelToIndex(adeleOutputLevel)
    }

    // Context-window fill colour (#76): KDE semantic hues per fill level.
    function contextLevelColor(level) {
        switch (level) {
            case "red": return Kirigami.Theme.negativeTextColor
            case "amber": return Kirigami.Theme.neutralTextColor
            case "green": return Kirigami.Theme.positiveTextColor
            default: return themeDisabledTextColor
        }
    }

    // ======================================================================
    //  model picker + background tasks
    // ======================================================================
    function modelLabel(item) {
        const conn = String(item.connection_label || item.connection_id || "")
        const m = item.model || {}
        const name = String(m.display_name || m.id || "")
        return conn.length > 0 ? (conn + " · " + name) : name
    }
    function activeSelection() {
        // The conversation's stored selection wins; otherwise the purpose default.
        if (currentSelection && currentSelection.connection_id) {
            return currentSelection
        }
        if (defaultModel && defaultModel.connection_id) {
            return defaultModel
        }
        return null
    }
    function modelChoiceIndex(sel) {
        if (!sel) {
            return -1
        }
        for (let i = 0; i < modelChoices.length; i++) {
            const it = modelChoices[i]
            const m = it.model || {}
            if (String(it.connection_id) === String(sel.connection_id)
                    && String(m.id) === String(sel.model_id)) {
                return i
            }
        }
        return -1
    }
    // Index 0 is the "(default)" sentinel (inherit); models follow at +1.
    function syncModelPicker() {
        const idx = modelChoiceIndex(activeSelection())
        modelSelectorCombo.currentIndex = idx >= 0 ? idx + 1 : 0
    }

    function countRunning(list) {
        let n = 0
        for (let i = 0; i < list.length; i++) {
            const s = String(list[i].status || "")
            if (s === "pending" || s === "running") {
                n += 1
            }
        }
        return n
    }
    function applyTasksReplaceAll(items) {
        tasksList = items
        runningTaskCount = countRunning(items)
    }
    function applyTaskStarted(task) {
        if (!task || !task.id) {
            return
        }
        const next = tasksList.filter(function(t) { return t.id !== task.id })
        next.push(task)
        tasksList = next
        runningTaskCount = countRunning(next)
    }
    function applyTaskProgress(id, hint) {
        if (id.length === 0) {
            return
        }
        tasksList = tasksList.map(function(t) {
            return t.id === id ? Object.assign({}, t, { progress_hint: hint }) : t
        })
    }
    function applyTaskCompleted(id) {
        if (id.length === 0) {
            return
        }
        tasksList = tasksList.map(function(t) {
            if (t.id !== id) {
                return t
            }
            const stillRunning = t.status === "running" || t.status === "pending"
            return Object.assign({}, t, { status: stillRunning ? "completed" : t.status })
        })
        runningTaskCount = countRunning(tasksList)
    }
    function formatLogEntry(entry) {
        if (!entry) {
            return ""
        }
        return "[" + String(entry.level || "info") + "] " + String(entry.message || "")
    }
    function applyTaskLogAppended(id, entry) {
        if (id.length === 0 || !entry) {
            return
        }
        const prev = taskLogsById[id] || ""
        const line = formatLogEntry(entry)
        // QV4-safe: assign to the GC root first, then write the key.
        taskLogsById = Object.assign({}, taskLogsById)
        taskLogsById[id] = prev.length > 0 ? (prev + "\n" + line) : line
    }
    function applyTaskLogs(id, entries) {
        if (id.length === 0) {
            return
        }
        const lines = []
        for (let i = 0; i < entries.length; i++) {
            lines.push(formatLogEntry(entries[i]))
        }
        taskLogsById = Object.assign({}, taskLogsById)
        taskLogsById[id] = lines.join("\n")
    }
    function findConversationIdForTask(taskId) {
        for (let i = 0; i < tasksList.length; i++) {
            const task = tasksList[i]
            if (!task || task.id !== taskId) {
                continue
            }
            const kind = task.kind || {}
            if (kind.conversation && kind.conversation.conversation_id) {
                return String(kind.conversation.conversation_id)
            }
            if (kind.subagent && kind.subagent.conversation_id) {
                return String(kind.subagent.conversation_id)
            }
            if (kind.standalone && kind.standalone.conversation_id) {
                return String(kind.standalone.conversation_id)
            }
        }
        return ""
    }
    function openTaskConversation(taskId) {
        const target = findConversationIdForTask(taskId)
        if (target.length > 0) {
            selectConversationById(target)
        }
    }

    // ======================================================================
    //  pure view helpers (no daemon / no transport)
    // ======================================================================
    function toImageSource(pathValue) {
        const value = String(pathValue || "").trim()
        if (value.length === 0) {
            return ""
        }
        if (value.indexOf("file://") === 0 || value.indexOf("image://") === 0 || value.indexOf("qrc:/") === 0 || value.indexOf(":/") === 0) {
            return value
        }
        if (value[0] === "/") {
            return "file://" + value
        }
        return value
    }

    function userAvatarCandidates() {
        const candidates = []
        const configured = toImageSource(configuredUserAvatarPath)
        if (configured.length > 0) {
            candidates.push(configured)
        }
        if (accountName.length > 0) {
            candidates.push(toImageSource("/var/lib/AccountsService/icons/" + accountName))
        }
        candidates.push(toImageSource(homeDirectory + "/.face.icon"))
        candidates.push(toImageSource(homeDirectory + "/.face"))
        return candidates
    }

    function markdownListLineCount(textValue) {
        const normalized = String(textValue === undefined || textValue === null ? "" : textValue)
            .replace(/\r\n/g, "\n").replace(/\r/g, "\n")
        if (normalized.length === 0) {
            return 0
        }
        const lines = normalized.split("\n")
        let count = 0
        for (let i = 0; i < lines.length; i++) {
            if (/^\s{0,3}(?:[-*+]|\d+[.)])\s+/.test(lines[i])) {
                count = count + 1
            }
        }
        return count
    }

    // Render assistant text as Markdown unless it's a very large list (which Qt's
    // MarkdownText lays out slowly enough to stutter the shell).
    function shouldRenderAssistantAsMarkdown(textValue) {
        const normalized = String(textValue === undefined || textValue === null ? "" : textValue)
        if (normalized.length === 0) {
            return false
        }
        const listLines = markdownListLineCount(normalized)
        const hasLargeList = listLines >= 45 && normalized.length >= 1800
        return !hasLargeList
    }

    function keepPromptCursorVisible() {
        if (!promptInput || !promptInputScroll) {
            return
        }
        const flickable = promptInputScroll.contentItem
        if (!flickable) {
            return
        }
        const caretTop = Number(promptInput.cursorRectangle.y || 0)
        const caretHeight = Number(promptInput.cursorRectangle.height || 0)
        const caretBottom = caretTop + caretHeight
        const viewportTop = Number(flickable.contentY || 0)
        const viewportHeight = Math.max(1, Number(flickable.height || promptInputScroll.height || 0))
        const viewportBottom = viewportTop + viewportHeight
        if (caretBottom > viewportBottom) {
            flickable.contentY = Math.max(0, caretBottom - viewportHeight)
        } else if (caretTop < viewportTop) {
            flickable.contentY = Math.max(0, caretTop)
        }
    }

    function zoomInUi() {
        uiScale = Math.min(maxUiScale, uiScale + zoomStep)
    }
    function zoomOutUi() {
        uiScale = Math.max(minUiScale, uiScale - zoomStep)
    }
    function resetZoomUi() {
        uiScale = 1.0
    }

    // ======================================================================
    //  models
    // ======================================================================
    // The transcript. Roles: kind ("message" | "note"), role ("user" |
    // "assistant"), body (text). A ListModel (C++-backed) sidesteps the QV4
    // insertMember GC hazard that a `property var` array of objects carries.
    ListModel {
        id: transcriptModel
    }

    // Queued outbound messages (submit order) shown as removable/editable chips.
    // Role: text (the queued prompt). A ListModel (C++-backed) sidesteps the QV4
    // insertMember GC hazard a `property var` array of objects would carry.
    ListModel {
        id: queuedMessagesModel
    }

    // ======================================================================
    //  shortcuts
    // ======================================================================
    Shortcut { sequence: "Ctrl++"; context: Qt.WindowShortcut; onActivated: root.zoomInUi() }
    Shortcut { sequence: "Ctrl+="; context: Qt.WindowShortcut; onActivated: root.zoomInUi() }
    Shortcut { sequence: "Ctrl+-"; context: Qt.WindowShortcut; onActivated: root.zoomOutUi() }
    Shortcut { sequence: "Ctrl+_"; context: Qt.WindowShortcut; onActivated: root.zoomOutUi() }
    Shortcut { sequence: "Ctrl+0"; context: Qt.WindowShortcut; onActivated: root.resetZoomUi() }

    // ======================================================================
    //  visual tree
    // ======================================================================
    Rectangle {
        anchors.fill: parent
        color: root.themeBackgroundColor
        border.width: 1
        border.color: root.themeDisabledTextColor
        radius: 8
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Image {
                // "Thinking" avatar whenever Adele is working — a text turn is in
                // flight (busy) OR the voice pipeline is Processing.
                source: (root.busy || root.voiceProcessing)
                    ? Qt.resolvedUrl("../images/adele_thinking.png")
                    : Qt.resolvedUrl("../images/adele.png")
                sourceSize.width: root.scaledTopIconSize
                sourceSize.height: root.scaledTopIconSize
                fillMode: Image.PreserveAspectFit
                Layout.preferredWidth: root.scaledTopIconSize
                Layout.preferredHeight: root.scaledTopIconSize
            }

            QQC2.Label {
                text: "Adele"
                font.bold: true
                color: root.themeTextColor
                Layout.fillWidth: true
            }

            // Context-window fill indicator (#76): a glanceable "12k / 32k (38%)"
            // readout, coloured green/amber/red by the level the core computed.
            QQC2.Label {
                visible: root.contextUsage !== null && !root.ultraNarrow
                text: root.contextUsage ? String(root.contextUsage.readout || "") : ""
                color: root.contextLevelColor(root.contextUsage ? String(root.contextUsage.level || "") : "")
                font.pointSize: root.baseFontPointSize * 0.85
                Layout.alignment: Qt.AlignVCenter
                QQC2.ToolTip.text: "Context window" + (root.contextUsage && root.contextUsage.compaction_active ? " — compaction active" : "")
                QQC2.ToolTip.visible: contextHover.hovered
                HoverHandler { id: contextHover }
            }

            TasksBadge {
                id: headerTasksBadge
                backend: root.tasksBackend
                Layout.alignment: Qt.AlignVCenter
                onClicked: root.tasksBadgeClicked()
            }
        }

        Flow {
            id: conversationControls
            Layout.fillWidth: true
            spacing: 6

            QQC2.ComboBox {
                id: conversationPicker
                width: {
                    const buttonWidths = newButton.implicitWidth + conversationControls.spacing
                    return Math.max(root.ultraNarrow ? 160 : 200, conversationControls.width - buttonWidths - conversationControls.spacing)
                }
                enabled: !root.busy
                model: root.conversationChoices
                textRole: "title"
                delegate: QQC2.ItemDelegate {
                    id: conversationDelegate
                    required property var modelData
                    required property int index
                    width: conversationPicker.width
                    highlighted: conversationPicker.highlightedIndex === index
                    background: Rectangle {
                        color: conversationDelegate.highlighted ? root.themeHighlightColor : "transparent"
                    }
                    contentItem: RowLayout {
                        spacing: 6
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: conversationDelegate.modelData.title
                            color: conversationDelegate.highlighted ? root.themeHighlightedTextColor : root.themeTextColor
                            elide: Text.ElideRight
                        }
                        QQC2.ToolButton {
                            icon.name: "edit-delete"
                            display: QQC2.AbstractButton.IconOnly
                            enabled: !root.busy
                            onClicked: {
                                core.deleteConversation(conversationDelegate.modelData.id)
                                conversationPicker.popup.close()
                            }
                        }
                    }
                    onClicked: {
                        conversationPicker.currentIndex = conversationDelegate.index
                        conversationPicker.popup.close()
                        root.selectConversationById(conversationDelegate.modelData.id)
                    }
                }
                onActivated: function(index) {
                    if (index >= 0 && index < root.conversationChoices.length) {
                        root.selectConversationById(root.conversationChoices[index].id)
                    }
                }
            }

            QQC2.Button {
                id: newButton
                text: "New"
                enabled: !root.busy
                onClicked: core.newConversation()
            }
        }

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: transcript
                model: transcriptModel
                spacing: 6
                clip: true

                header: Column {
                    width: transcript.width
                    visible: transcriptModel.count === 0
                    spacing: 8
                    topPadding: 40

                    Image {
                        source: Qt.resolvedUrl("../images/adele.png")
                        sourceSize.width: root.scaledHeaderIconSize
                        sourceSize.height: root.scaledHeaderIconSize
                        width: root.scaledHeaderIconSize
                        height: root.scaledHeaderIconSize
                        fillMode: Image.PreserveAspectFit
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                    QQC2.Label {
                        text: "Hi! I'm Adele! Ask me anything…"
                        font.pointSize: root.baseFontPointSize * root.uiScale
                        color: root.themeDisabledTextColor
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                }

                delegate: Item {
                    id: messageEntry
                    required property string kind
                    required property string role
                    required property string body
                    readonly property bool isNote: kind === "note"
                    readonly property bool isAssistant: role === "assistant"
                    readonly property bool renderAssistantAsMarkdown: isAssistant && !isNote && root.shouldRenderAssistantAsMarkdown(body)
                    readonly property real avatarSize: root.transcriptAvatarSize
                    readonly property real bubbleWidth: isNote ? root.transcriptWideBubbleWidth : root.transcriptMessageBubbleWidth
                    readonly property var avatarSources: isAssistant ? [root.adeleAvatarSource] : root.userAvatarCandidates()

                    width: ListView.view.width
                    implicitHeight: rowContainer.implicitHeight + 2

                    RowLayout {
                        id: rowContainer
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        spacing: 6
                        layoutDirection: messageEntry.isNote ? Qt.LeftToRight : (messageEntry.isAssistant ? Qt.RightToLeft : Qt.LeftToRight)

                        Item {
                            Layout.preferredWidth: messageEntry.isNote ? 0 : messageEntry.avatarSize
                            Layout.preferredHeight: messageEntry.isNote ? 0 : messageEntry.avatarSize
                            Layout.alignment: Qt.AlignTop
                            visible: !messageEntry.isNote

                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                color: root.themeBackgroundColor
                                border.width: 1
                                border.color: root.themeDisabledTextColor
                                clip: true

                                Image {
                                    id: avatarImage
                                    property int candidateIndex: 0
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.top: parent.top
                                    width: messageEntry.isAssistant ? parent.width * 1.9 : parent.width
                                    height: messageEntry.isAssistant ? parent.height * 1.9 : parent.height
                                    fillMode: Image.PreserveAspectCrop
                                    horizontalAlignment: Image.AlignHCenter
                                    verticalAlignment: messageEntry.isAssistant ? Image.AlignTop : Image.AlignVCenter
                                    source: messageEntry.avatarSources.length > 0
                                        ? messageEntry.avatarSources[Math.min(candidateIndex, messageEntry.avatarSources.length - 1)]
                                        : ""
                                    visible: status === Image.Ready
                                    onStatusChanged: {
                                        if (status === Image.Error && candidateIndex < messageEntry.avatarSources.length - 1) {
                                            candidateIndex += 1
                                        }
                                    }
                                }
                                Kirigami.Icon {
                                    anchors.fill: parent
                                    source: messageEntry.isAssistant ? "preferences-desktop-user" : "user-identity"
                                    visible: !avatarImage.visible
                                }
                            }
                        }

                        Rectangle {
                            id: bubble
                            Layout.fillWidth: true
                            Layout.maximumWidth: messageEntry.bubbleWidth
                            Layout.alignment: Qt.AlignTop
                            Layout.preferredWidth: messageEntry.bubbleWidth
                            implicitWidth: Layout.preferredWidth
                            implicitHeight: bubbleContent.implicitHeight + 12
                            height: implicitHeight
                            radius: messageEntry.isNote ? 0 : 8
                            color: messageEntry.isNote
                                ? "transparent"
                                : (messageEntry.isAssistant ? root.themeBackgroundColor : root.themeHighlightColor)
                            border.width: messageEntry.isNote ? 0 : 1
                            border.color: root.themeDisabledTextColor

                            ColumnLayout {
                                id: bubbleContent
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 6
                                spacing: 4

                                TextEdit {
                                    id: messageText
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: contentHeight
                                    readOnly: true
                                    selectByMouse: true
                                    selectByKeyboard: true
                                    wrapMode: TextEdit.Wrap
                                    textFormat: (messageEntry.isAssistant && messageEntry.renderAssistantAsMarkdown)
                                        ? Text.MarkdownText
                                        : Text.PlainText
                                    text: messageEntry.body
                                    color: messageEntry.isNote
                                        ? root.themeDisabledTextColor
                                        : (messageEntry.isAssistant ? root.themeTextColor : root.themeHighlightedTextColor)
                                    font.pointSize: root.baseFontPointSize * root.uiScale
                                    font.italic: messageEntry.isNote
                                    activeFocusOnPress: true
                                    selectedTextColor: messageEntry.isAssistant ? root.themeHighlightedTextColor : root.themeTextColor
                                    selectionColor: messageEntry.isAssistant ? root.themeHighlightColor : root.themeBackgroundColor
                                    onLinkActivated: function(link) {
                                        // #11: assistant text is MarkdownText; gate URLs on a
                                        // scheme allowlist before handing them to the system.
                                        LinkSafety.openLinkSafely(link, Qt.openUrlExternally)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Transient chat status ("Thinking…") — collapses to nothing at idle.
        QQC2.Label {
            Layout.fillWidth: true
            visible: root.chatStatusText.length > 0
            text: root.chatStatusText
            font.italic: true
            color: root.themeDisabledTextColor
            elide: Text.ElideRight
        }

        // Queued messages: prompts submitted while a reply was still streaming.
        // They are held (not sent) until the stream ends, then flushed as one
        // combined turn. Each chip previews a queued message; tapping it recalls
        // the message into the composer to edit (an in-place reinsert on
        // re-submit), the x drops it. The count is the "N queued" indicator.
        Flow {
            id: queuedChipsRow
            Layout.fillWidth: true
            visible: queuedMessagesModel.count > 0
            spacing: Math.round(6 * root.uiScale)

            QQC2.Label {
                height: Math.round(26 * root.uiScale)
                verticalAlignment: Text.AlignVCenter
                // Row-level cue: while a message is checked out for editing it is
                // in the composer, not the visible chips, so no single chip is
                // "the" edited one. Flag the state here instead.
                text: root.editingQueuedIndex >= 0
                    ? queuedMessagesModel.count + " queued, editing"
                    : queuedMessagesModel.count + " queued"
                color: root.editingQueuedIndex >= 0 ? root.themeHighlightColor : root.themeDisabledTextColor
                font.bold: true
                font.pointSize: root.baseFontPointSize * 0.9
            }

            Repeater {
                model: queuedMessagesModel

                // The chip's click->index mapping and remove wiring live in
                // QueuedChip.qml so they can be exercised headless in
                // qmltestrunner (ChatView can't be instantiated there). The
                // delegate injects the model's index/text plus theme styling and
                // routes the chip's signals to the core.
                delegate: QueuedChip {
                    required property int index
                    required property string text

                    chipIndex: index
                    chipText: text
                    editingIndex: root.editingQueuedIndex
                    uiScale: root.uiScale
                    baseFontPointSize: root.baseFontPointSize

                    onEditRequested: function(fullIndex) { core.editQueued(fullIndex) }
                    onRemoveRequested: function(idx) { core.removeQueued(idx) }
                }
            }
        }

        // TTS voice switcher. Hidden unless the voice daemon is up AND at least
        // one voice is installed, so it never shows a dead control. The speaker
        // picker only appears for multi-speaker voices.
        RowLayout {
            id: voiceSwitcherRow
            Layout.fillWidth: true
            visible: voice.available && voice.voices.length > 0 && !root.ultraNarrow
            spacing: 6

            QQC2.Label {
                text: "Voice:"
                color: root.themeDisabledTextColor
                Layout.alignment: Qt.AlignVCenter
            }

            QQC2.ComboBox {
                id: voiceSelectorCombo
                Layout.fillWidth: true
                enabled: !root.busy && voice.voices.length > 0
                model: voice.voices
                textRole: "label"
                currentIndex: Math.max(0, root.voiceIndexById(voice.voiceId))
                onActivated: function(index) {
                    if (index < 0 || index >= voice.voices.length) {
                        return
                    }
                    // Switching voice resets the speaker to default (-1).
                    voice.setVoice(voice.voices[index].voice_id, -1)
                }
            }

            QQC2.ComboBox {
                id: voiceSpeakerCombo
                visible: root.voiceSpeakerCount(voice.voiceId) > 1
                Layout.preferredWidth: 110
                enabled: !root.busy
                model: {
                    const count = root.voiceSpeakerCount(voice.voiceId)
                    const labels = []
                    for (let i = 0; i < count; i++) {
                        labels.push("Speaker " + i)
                    }
                    return labels
                }
                currentIndex: voice.speakerId >= 0 ? voice.speakerId : 0
                onActivated: function(index) {
                    voice.setVoice(voice.voiceId, index)
                }
            }
        }

        // Per-conversation voice mode (#80). You = speak to Adele (push-to-talk +
        // reply narration); Adele = how often she narrates back. Shown when the
        // voice daemon is up. The model can also drive Adele via
        // request_voice / stop_voice — the adele_output_dropdown event reflects it.
        RowLayout {
            id: voiceModeRow
            Layout.fillWidth: true
            visible: voice.available && !root.ultraNarrow
            spacing: 6

            QQC2.Label {
                text: "You:"
                color: root.themeDisabledTextColor
                Layout.alignment: Qt.AlignVCenter
            }
            QQC2.ComboBox {
                id: youCombo
                Layout.fillWidth: true
                enabled: !root.busy && root.conversationId.length > 0
                model: ["Off", "On"]
                onActivated: function(index) { root.setVoiceInForCurrent(index === 1) }
            }
            QQC2.Label {
                text: "Adele:"
                color: root.themeDisabledTextColor
                Layout.alignment: Qt.AlignVCenter
            }
            QQC2.ComboBox {
                id: adeleCombo
                Layout.fillWidth: true
                enabled: !root.busy && root.conversationId.length > 0
                model: ["Off", "On demand", "Always"]
                onActivated: function(index) { root.setAdeleOutputForCurrent(root.adeleIndexToLevel(index)) }
            }
        }

        // Per-conversation model picker. Index 0 is the "(default)" sentinel
        // (inherit the conversation / interactive-purpose default); the
        // connection·model listings follow. Shown when the core advertises models.
        RowLayout {
            id: modelSelectorRow
            Layout.fillWidth: true
            visible: (root.modelPickerVisible || root.modelChoices.length > 0) && !root.ultraNarrow
            spacing: 6

            QQC2.Label {
                text: "Model:"
                color: root.themeDisabledTextColor
                Layout.alignment: Qt.AlignVCenter
            }
            QQC2.ComboBox {
                id: modelSelectorCombo
                Layout.fillWidth: true
                enabled: !root.busy && root.conversationId.length > 0 && root.modelChoices.length > 0
                model: {
                    const labels = ["(default)"]
                    for (let i = 0; i < root.modelChoices.length; i++) {
                        labels.push(root.modelLabel(root.modelChoices[i]))
                    }
                    return labels
                }
                onActivated: function(index) {
                    if (index <= 0) {
                        core.selectModel("", "", "")
                    } else {
                        const it = root.modelChoices[index - 1]
                        core.selectModel(String(it.connection_id), String((it.model || {}).id || ""), "")
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            // Push-to-talk toggle: starts a dictation turn from Idle (works even
            // with the wake word off) and stops the active turn otherwise. A
            // coloured, breathing ring makes an open mic unmissable. Hidden when
            // the voice daemon isn't available so it never dangles.
            QQC2.ToolButton {
                id: micButton
                visible: voice.available
                enabled: voice.available && !root.busy
                Layout.alignment: Qt.AlignTop
                icon.name: root.voiceStateIcon
                highlighted: root.voiceActive
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: root.micButtonTooltip()
                QQC2.ToolTip.visible: hovered
                onClicked: root.voiceMicToggle()

                Rectangle {
                    id: micStateRing
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: Math.round(Math.min(width, height) / 4)
                    color: "transparent"
                    visible: root.voiceActive
                    border.width: Math.max(2, Math.round(2 * root.uiScale))
                    border.color: root.voiceStateColor
                    opacity: (root.voiceListening || root.voiceProcessing) ? 1.0 : 0.9
                    SequentialAnimation on opacity {
                        running: micStateRing.visible && root.voiceListening
                        loops: Animation.Infinite
                        alwaysRunToEnd: true
                        NumberAnimation { from: 1.0; to: 0.35; duration: 750; easing.type: Easing.InOutSine }
                        NumberAnimation { from: 0.35; to: 1.0; duration: 750; easing.type: Easing.InOutSine }
                    }
                    SequentialAnimation on opacity {
                        running: micStateRing.visible && root.voiceProcessing
                        loops: Animation.Infinite
                        alwaysRunToEnd: true
                        NumberAnimation { from: 1.0; to: 0.5; duration: 1100; easing.type: Easing.InOutSine }
                        NumberAnimation { from: 0.5; to: 1.0; duration: 1100; easing.type: Easing.InOutSine }
                    }
                }
            }

            // Dedicated "cancel this turn" affordance — only ever stops. Visible
            // only while a turn is in flight so it never dangles at Idle.
            QQC2.ToolButton {
                id: voiceCancelButton
                visible: root.voiceActive
                enabled: root.voiceActive && !root.busy
                Layout.alignment: Qt.AlignTop
                icon.name: "dialog-cancel"
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: "Stop — cancel the current turn"
                QQC2.ToolTip.visible: hovered
                onClicked: root.voiceCancelTurn()
            }

            // Conversational-state chip: spells the state in words with a
            // state-coloured, pulsing dot. Shown only while a turn is active.
            Rectangle {
                id: voiceStateChip
                visible: root.voiceActive
                Layout.alignment: Qt.AlignVCenter
                implicitHeight: Math.round(24 * root.uiScale)
                implicitWidth: voiceStateChipRow.implicitWidth + Math.round(16 * root.uiScale)
                radius: implicitHeight / 2
                color: Qt.rgba(root.voiceStateColor.r, root.voiceStateColor.g, root.voiceStateColor.b, 0.12)
                border.width: 1
                border.color: Qt.rgba(root.voiceStateColor.r, root.voiceStateColor.g, root.voiceStateColor.b, 0.45)

                RowLayout {
                    id: voiceStateChipRow
                    anchors.centerIn: parent
                    spacing: Math.round(6 * root.uiScale)

                    Rectangle {
                        id: voiceStateDot
                        implicitWidth: Math.round(8 * root.uiScale)
                        implicitHeight: implicitWidth
                        radius: implicitWidth / 2
                        color: root.voiceStateColor
                        Layout.alignment: Qt.AlignVCenter
                        SequentialAnimation on opacity {
                            running: voiceStateChip.visible && root.voiceListening
                            loops: Animation.Infinite
                            alwaysRunToEnd: true
                            NumberAnimation { from: 1.0; to: 0.3; duration: 750; easing.type: Easing.InOutSine }
                            NumberAnimation { from: 0.3; to: 1.0; duration: 750; easing.type: Easing.InOutSine }
                        }
                        SequentialAnimation on opacity {
                            running: voiceStateChip.visible && root.voiceProcessing
                            loops: Animation.Infinite
                            alwaysRunToEnd: true
                            NumberAnimation { from: 1.0; to: 0.4; duration: 1100; easing.type: Easing.InOutSine }
                            NumberAnimation { from: 0.4; to: 1.0; duration: 1100; easing.type: Easing.InOutSine }
                        }
                    }

                    QQC2.Label {
                        text: root.voiceStateLabel
                        color: root.voiceStateColor
                        font.bold: true
                        font.pointSize: root.baseFontPointSize
                        Layout.alignment: Qt.AlignVCenter
                    }
                }
            }

            QQC2.ScrollView {
                id: promptInputScroll
                Layout.fillWidth: true
                Layout.preferredHeight: Math.round(72 * root.uiScale)
                Layout.maximumHeight: Math.round(180 * root.uiScale)
                clip: true
                QQC2.ScrollBar.horizontal.policy: QQC2.ScrollBar.AlwaysOff
                QQC2.ScrollBar.vertical.policy: QQC2.ScrollBar.AsNeeded

                QQC2.TextArea {
                    id: promptInput
                    width: promptInputScroll.availableWidth
                    placeholderText: "Ask Adele…"
                    wrapMode: TextEdit.Wrap
                    // Always editable — a submit while a reply streams is queued
                    // by the core, not blocked.
                    onTextChanged: Qt.callLater(root.keepPromptCursorVisible)
                    onCursorPositionChanged: Qt.callLater(root.keepPromptCursorVisible)
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            Qt.callLater(root.keepPromptCursorVisible)
                        }
                    }
                    Keys.onPressed: function(event) {
                        // Up/Down recall a queued message into the composer to
                        // edit it. Only fires when the field is empty (Up walks
                        // backward from the last; Down walks forward or cancels
                        // the edit) so it never fights caret movement.
                        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                            const decision = QueueRecall.recallDecision(
                                event.key === Qt.Key_Up ? "up" : "down",
                                promptInput.length === 0,
                                root.editingQueuedIndex,
                                queuedMessagesModel.count)
                            if (decision.action === "edit") {
                                core.editQueued(decision.index)
                                event.accepted = true
                            } else if (decision.action === "cancel") {
                                core.cancelQueuedEdit()
                                event.accepted = true
                            }
                            return
                        }
                        const isEnterKey = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        if (!isEnterKey) {
                            return
                        }
                        if (event.modifiers & Qt.MetaModifier) {
                            insert(cursorPosition, "\n")
                            event.accepted = true
                            return
                        }
                        root.submitPrompt()
                        event.accepted = true
                    }
                }
            }
        }

        Flow {
            id: actionControls
            Layout.fillWidth: true
            spacing: 6

            QQC2.Button {
                id: sendButton
                // Enabled on connection, not on idleness: a submit while a reply
                // streams is queued. The label hints at that ("Queue" vs "Send").
                text: root.busy ? "Queue" : "Send"
                enabled: core.connected && root.conversationId.length > 0
                onClicked: root.submitPrompt()
            }

            QQC2.Button {
                text: "Refresh"
                visible: !panelMode
                enabled: !root.busy && root.conversationId.length > 0
                onClicked: root.selectConversationById(root.conversationId)
            }

            QQC2.Label {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                visible: root.statusText.length > 0
                text: root.statusText
                color: root.themeDisabledTextColor
                elide: Text.ElideRight
                font.pointSize: root.baseFontPointSize * 0.9
            }
        }
    }

    Component.onCompleted: {
        core.connectToDaemon("dbus")
        voice.start()
    }
}
