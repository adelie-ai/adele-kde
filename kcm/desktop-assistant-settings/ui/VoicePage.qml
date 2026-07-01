/*
 * Voice settings page (adele-kde#30).
 *
 * Parallel to Connections / Knowledge — NOT a "purpose" (STT/TTS aren't LLM
 * connections). Surfaces the voice daemon's settings (repo adelie-ai/voice,
 * D-Bus name org.desktopAssistant.Voice — distinct from the orchestrator).
 *
 * Split by how each setting is actually applied, because the daemon exposes
 * only a slice of this over D-Bus:
 *   * Live (D-Bus, hot): "Enable 'Hey Adele'" + TTS voice/speaker selection.
 *   * Config file (~/.config/adele-voice/config.toml; takes effect on the next
 *     daemon (re)start): STT language, wake sensitivity, input/output device.
 *   * systemd user unit: "Allow autostart" (enable/disable adele-voice).
 *
 * All daemon access happens C++-side (kcm.*); this page is declarative and
 * binds to KCM properties. The whole page disables when the voice service
 * isn't on the bus and the unit isn't installed (graceful degradation).
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

import "VoiceBackends.js" as VoiceBackends
import "AudioDevices.js" as AudioDevices
import "WhisperModels.js" as WhisperModels

ColumnLayout {
    id: root
    spacing: 12

    // The voice feature is "present" if either the live daemon is on the bus
    // OR its config/unit exists locally (so you can pre-configure before first
    // launch). When neither is true the page shows an explainer and disables.
    readonly property bool voicePresent: kcm.voiceServiceAvailable || kcm.voiceAutostart >= 0

    // Last-measured input peak (0..1) from the "Test" button; -1 = not measured
    // yet (or no level could be taken). Drives the mic-level meter + nudge.
    property real micLevel: -1

    function voiceIndexById(id) {
        const list = kcm.voiceList || []
        for (let i = 0; i < list.length; i++) {
            if (list[i].voice_id === id) {
                return i
            }
        }
        return -1
    }

    // Backend / Polly-engine token<->index mapping lives in VoiceBackends.js so
    // it can be unit-tested without the C++ `kcm` context (see
    // tests/qml/tst_VoiceBackends.qml). These thin wrappers keep the existing
    // `root.*` call sites and the binding-friendly signature.
    function backendIndexById(id) {
        return VoiceBackends.backendIndexById(id)
    }

    function pollyEngineIndexById(id) {
        return VoiceBackends.pollyEngineIndexById(id)
    }

    function listeningCueIndexById(id) {
        return VoiceBackends.listeningCueIndexById(id)
    }

    function speakerCountFor(id) {
        const list = kcm.voiceList || []
        const idx = voiceIndexById(id)
        if (idx < 0) {
            return 1
        }
        return Math.max(1, Number(list[idx].num_speakers || 1))
    }

    function voiceLabel(entry) {
        const name = String(entry.display_name || entry.voice_id || "")
        const lang = String(entry.language || "")
        return lang.length > 0 ? (name + " — " + lang) : name
    }

    // Map a stored device value (e.g. "default" or an ALSA card token) to its
    // row in one of the kcm device-option lists. Pure logic lives in
    // AudioDevices.js so it can be unit-tested without the C++ `kcm` context
    // (see tests/qml/tst_AudioDevices.qml); this thin wrapper keeps the
    // binding-friendly call site.
    function deviceIndexByValue(options, value) {
        return AudioDevices.deviceIndexByValue(options, value)
    }

    // --- Whisper STT model selector (adele-kde#44) --------------------------
    // The catalog + file<->row + custom-path logic lives in WhisperModels.js so
    // it can be unit-tested without the C++ `kcm` context (see
    // tests/qml/tst_WhisperModels.qml). The page composes the ComboBox model
    // from the catalog, plus (1) an optional leading "Custom: <path>" row when
    // the configured path is hand-edited / not in the catalog, and (2) a trailing
    // "Other / custom path…" row that reveals the free-form field.
    //
    // sttUseCustom drives that free-form mode: true when the user explicitly
    // picks "Other / custom path…", OR when the configured path is a custom one
    // (so a hand-edited config.toml round-trips losslessly — opening + closing
    // the KCM leaves it untouched).
    property bool sttUseCustom: WhisperModels.isCustomPath(kcm.sttModelPath)

    // The configured path's basename is a hand-edited/custom one.
    readonly property bool sttPathIsCustom: WhisperModels.isCustomPath(kcm.sttModelPath)

    // ComboBox row objects. Each is { key, file, label, url, custom }.
    //   * one row per catalog model (with a "(not downloaded)" suffix when
    //     missing from disk),
    //   * a leading "Custom: <path>" row iff the configured path is custom,
    //   * a trailing "Other / custom path…" row (key "other").
    function sttModelRows() {
        const rows = []
        if (root.sttPathIsCustom) {
            rows.push({
                key: "custom-current",
                file: "",
                label: "Custom: " + kcm.sttModelPath,
                url: "",
                custom: true,
            })
        }
        const models = WhisperModels.MODELS
        for (let i = 0; i < models.length; i++) {
            const m = models[i]
            const present = kcm.sttModelInstalled(m.file)
            rows.push({
                key: m.file,
                file: m.file,
                label: present ? m.label : (m.label + " (not downloaded)"),
                url: m.url,
                custom: false,
            })
        }
        rows.push({
            key: "other",
            file: "",
            label: "Other / custom path…",
            url: "",
            custom: true,
        })
        return rows
    }

    // Row index in sttModelRows() that matches the current state: the custom
    // row when a custom path is configured, else the catalog row by basename.
    function sttCurrentIndex() {
        const rows = root.sttModelRows()
        if (root.sttPathIsCustom) {
            for (let i = 0; i < rows.length; i++) {
                if (rows[i].key === "custom-current") {
                    return i
                }
            }
        }
        const file = WhisperModels.basename(kcm.sttModelPath)
        for (let j = 0; j < rows.length; j++) {
            if (rows[j].key === file) {
                return j
            }
        }
        // No configured path (daemon default) -> first catalog row.
        for (let k = 0; k < rows.length; k++) {
            if (!rows[k].custom) {
                return k
            }
        }
        return 0
    }

    // The catalog entry currently selected, or null when on a custom row.
    function sttSelectedCatalogModel() {
        const file = WhisperModels.basename(kcm.sttModelPath)
        const idx = WhisperModels.modelIndexByFile(file)
        const m = WhisperModels.MODELS[idx]
        return (m && m.file === file) ? m : null
    }

    // A catalog model is selected but its file is not on disk.
    readonly property bool sttSelectedMissing: {
        if (root.sttPathIsCustom || kcm.sttModelPath.length === 0) {
            return false
        }
        const file = WhisperModels.basename(kcm.sttModelPath)
        const m = root.sttSelectedCatalogModel()
        return m !== null && !kcm.sttModelInstalled(file)
    }

    Component.onCompleted: {
        kcm.loadVoiceSettings()
        // Enumerate audio devices only when the Voice tab is actually shown —
        // it spawns pactl/arecord/aplay, which we don't want on every tab of the
        // settings module. loadVoiceSettings() above has already read the
        // configured device names so the "(configured)" fallback row is correct.
        kcm.loadAudioDevices()
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: !root.voicePresent
        type: Kirigami.MessageType.Information
        text: "The voice service isn't installed or running. Install the Adele "
              + "voice daemon (repo adelie-ai/voice) to enable “Hey Adele”, "
              + "dictation, and spoken replies."
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: root.voicePresent && !kcm.voiceServiceAvailable
        type: Kirigami.MessageType.Information
        text: "The voice service isn't running right now. You can still edit "
              + "its configuration below; changes apply the next time it starts."
    }

    // --- Wake word (live, D-Bus) --------------------------------------------
    QQC2.Label {
        font.bold: true
        text: "Wake word"
    }

    QQC2.CheckBox {
        id: enableHeyAdeleCheck
        text: "Enable “Hey Adele”"
        enabled: kcm.voiceServiceAvailable
        checked: kcm.voiceEnabled
        onToggled: kcm.voiceEnabled = checked
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "Always-on wake-word listening. Toggling this takes effect "
              + "immediately on the running service."
    }

    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Wake sensitivity"
            Layout.preferredWidth: 150
        }
        QQC2.Slider {
            id: sensitivitySlider
            Layout.fillWidth: true
            from: 0.0
            to: 1.0
            stepSize: 0.01
            value: kcm.wakeSensitivity
            // Commit on release, not per drag-tick (KDE-7 / #62): the label
            // below binds to `value` for live feedback while dragging, but the
            // model (which writes config.toml) is only updated when the handle
            // is released. `onMoved` (drag) deliberately does NOT write;
            // keyboard/click steps fire `moved` while not pressed, so those are
            // handled by the !pressed guard here.
            onPressedChanged: if (!pressed) kcm.wakeSensitivity = value
            onMoved: if (!pressed) kcm.wakeSensitivity = value
        }
        QQC2.Label {
            Layout.preferredWidth: 44
            horizontalAlignment: Text.AlignRight
            text: Number(sensitivitySlider.value).toFixed(2)
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        // What this value MEANS depends on the wake mode (the "Eager wake"
        // checkbox below), so the guidance is mode-specific. Describing both
        // cases in one blurb is what made the old text read as contradictory
        // ("lower if it's being missed" vs. "too low and it never triggers").
        text: kcm.wakeEager
            ? "How strong a match “Hey Adele” must be to wake Adele, from 0 to 1. "
              + "In eager mode it's a straightforward dial: LOWER wakes more easily "
              + "but risks false triggers; HIGHER is stricter but may miss you. "
              + "Values usually land around 0.2–0.4. Use “Calibrate automatically” "
              + "below to set it from your own voice and microphone."
            : "How strong a match “Hey Adele” must be to wake Adele, from 0 to 1. "
              + "In standard (non-eager) mode this is NOT a simple dial — it has a "
              + "sweet spot: too HIGH and it misses you, but too LOW and it never "
              + "fires at all (it waits for the match to drop back below this value "
              + "at the end of the phrase, which never happens if the value sits "
              + "below the room's background match level). Around 0.3–0.45 usually "
              + "works. Tip: turn on “Eager wake” below for a simpler dial where "
              + "lower always just means easier to trigger."
    }

    // Auto-calibration (#121): let the daemon measure the user's real "Hey
    // Adele" scores and set the threshold, instead of hand-tuning the slider.
    // Needs the service actually running (it takes over the mic), so gate on the
    // live `voiceServiceAvailable`, not `voicePresent`.
    RowLayout {
        Layout.fillWidth: true
        QQC2.Button {
            text: kcm.calibrationActive ? "Calibrating…" : "Calibrate automatically…"
            icon.name: "audio-input-microphone"
            enabled: kcm.voiceServiceAvailable && !kcm.calibrationActive
            onClicked: kcm.calibrateWake()
        }
        QQC2.BusyIndicator {
            running: kcm.calibrationActive
            visible: kcm.calibrationActive
            Layout.preferredHeight: parent.height
        }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.8
            visible: kcm.calibrationStatus.length > 0
            text: kcm.calibrationStatus
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "Calibrate picks the threshold for you: the assistant asks you to "
              + "say “Hey Adele” a few times and sets a value that matches your "
              + "voice and microphone. The result is applied immediately and "
              + "saved. Requires the voice service to be running."
    }

    // Eager wake (voice#50) + listening cue. `eager` is captured at daemon
    // startup, so a change needs a voice-service restart to take effect (unlike
    // sensitivity, which applies live).
    QQC2.CheckBox {
        id: wakeEagerCheck
        text: "Eager wake (fire as soon as the match crosses the sensitivity)"
        enabled: root.voicePresent
        checked: kcm.wakeEager
        onToggled: kcm.wakeEager = checked
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "Eager wake fires the instant the match score crosses the "
              + "sensitivity above, instead of waiting for you to finish the "
              + "phrase. It's snappier, and it makes sensitivity behave like a "
              + "simple dial (lower = easier to trigger) — so it pairs best with "
              + "“Calibrate automatically”, which sets a low per-voice value. The "
              + "trade-off is a bit more chance of false triggers. Changing this "
              + "takes effect after the voice service restarts."
    }

    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Listening cue"
            Layout.preferredWidth: 150
        }
        QQC2.ComboBox {
            id: listeningCueCombo
            Layout.fillWidth: true
            textRole: "label"
            // Order MUST match VoiceBackends.LISTENING_CUES (ding/phrase/off).
            model: [
                { label: "Ding (earcon)", value: "ding" },
                { label: "Spoken phrase", value: "phrase" },
                { label: "Off (no cue)", value: "off" }
            ]
            currentIndex: Math.max(0, root.listeningCueIndexById(kcm.listeningCue))
            onActivated: kcm.listeningCue = model[currentIndex].value
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "Audible cue the instant the wake word is heard: a short ding "
              + "(instant), a spoken phrase like “Yes?” (friendlier, adds ~1 s), "
              + "or off."
    }

    RowLayout {
        Layout.fillWidth: true
        QQC2.Button {
            text: "Reset wake-word defaults"
            icon.name: "edit-undo"
            enabled: root.voicePresent
            onClicked: kcm.resetWakeDefaults()
        }
        Item { Layout.fillWidth: true }
    }

    Kirigami.Separator { Layout.fillWidth: true }

    // --- Endpointing (config file: [vad] + [assistant]) ---------------------
    // How the daemon decides you've started and stopped talking. All three are
    // TOML-backed and applied via "Apply now" at the bottom of the page.
    QQC2.Label {
        font.bold: true
        text: "Endpointing"
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "Controls how the assistant detects the start and end of speech."
    }

    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Speech threshold"
            Layout.preferredWidth: 150
        }
        QQC2.Slider {
            id: speechThresholdSlider
            Layout.fillWidth: true
            from: 0.0
            to: 1.0
            stepSize: 0.01
            value: kcm.vadSpeechThreshold
            // Commit on release, live label during drag (KDE-7 / #62) — see the
            // wake-sensitivity slider above for the rationale.
            onPressedChanged: if (!pressed) kcm.vadSpeechThreshold = value
            onMoved: if (!pressed) kcm.vadSpeechThreshold = value
        }
        QQC2.Label {
            Layout.preferredWidth: 44
            horizontalAlignment: Text.AlignRight
            text: Number(speechThresholdSlider.value).toFixed(2)
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "How confident the voice-activity detector must be that you’re "
              + "speaking. Higher ignores more background noise but may clip "
              + "quiet speech; lower picks up softer speech but also more noise. "
              + "Default 0.50."
    }

    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Silence to end (ms)"
            Layout.preferredWidth: 150
        }
        QQC2.SpinBox {
            id: silenceDurationSpin
            Layout.fillWidth: true
            from: 0
            to: 20000
            stepSize: 250
            editable: true
            value: kcm.vadSilenceDurationMs
            onValueModified: kcm.vadSilenceDurationMs = value
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "How long the assistant keeps listening (lingers) after you stop "
              + "talking before it treats your turn as finished. Longer is more "
              + "forgiving of pauses; shorter feels snappier. Default 3000 ms."
    }

    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Follow-up wait (ms)"
            Layout.preferredWidth: 150
        }
        QQC2.SpinBox {
            id: followupTimeoutSpin
            Layout.fillWidth: true
            from: 0
            to: 60000
            stepSize: 500
            editable: true
            value: kcm.followupTimeoutMs
            onValueModified: kcm.followupTimeoutMs = value
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "In conversation mode, how long the assistant waits for you to "
              + "start a follow-up before ending the conversation and returning "
              + "to the wake word. Default 10000 ms."
    }

    RowLayout {
        Layout.fillWidth: true
        QQC2.Button {
            text: "Reset endpointing defaults"
            icon.name: "edit-undo"
            enabled: root.voicePresent
            onClicked: kcm.resetEndpointingDefaults()
        }
        Item { Layout.fillWidth: true }
    }

    Kirigami.Separator { Layout.fillWidth: true }

    // --- Text-to-speech (backend + per-backend config + voice picker) -------
    // The voice daemon (repo adelie-ai/voice) has pluggable TTS backends chosen
    // by `tts.backend` in config.toml: Kokoro (local, default) / Piper (local) /
    // Polly (AWS cloud, billable). The backend + per-backend keys are config —
    // applied on the next service start — while the voice picker below is live
    // over D-Bus and reflects whichever backend is currently running.
    QQC2.Label {
        font.bold: true
        text: "Text-to-speech"
    }

    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Backend"
            Layout.preferredWidth: 150
        }
        QQC2.ComboBox {
            id: ttsBackendCombo
            Layout.fillWidth: true
            textRole: "label"
            // Keep `value` (the config token) and the index aligned via
            // backendIndexById; onActivated writes the selected token.
            model: [
                { value: "kokoro", label: "Kokoro (local, default)" },
                { value: "piper", label: "Piper (local)" },
                { value: "polly", label: "Polly (cloud — billable)" },
            ]
            currentIndex: Math.max(0, root.backendIndexById(kcm.ttsBackend))
            onActivated: kcm.ttsBackend = model[currentIndex].value
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "Selects how spoken replies are synthesized. Applied the next time "
              + "the voice service starts — use “Restart voice service” below to "
              + "apply it now."
    }

    // Kokoro: espeak-ng phonemizer language (en-us / en-gb).
    RowLayout {
        Layout.fillWidth: true
        visible: kcm.ttsBackend === "kokoro"
        enabled: root.voicePresent
        QQC2.Label {
            text: "Kokoro language"
            Layout.preferredWidth: 150
        }
        QQC2.TextField {
            id: kokoroLangField
            Layout.fillWidth: true
            placeholderText: "en-us"
            text: kcm.kokoroLang
            onEditingFinished: kcm.kokoroLang = text
        }
    }

    // Piper: path to the voice model (.onnx). Empty = daemon default.
    RowLayout {
        Layout.fillWidth: true
        visible: kcm.ttsBackend === "piper"
        enabled: root.voicePresent
        QQC2.Label {
            text: "Voice model path"
            Layout.preferredWidth: 150
        }
        QQC2.TextField {
            id: piperModelPathField
            Layout.fillWidth: true
            placeholderText: "~/.local/share/adele-voice/models/en_US-amy-medium.onnx"
            text: kcm.piperModelPath
            onEditingFinished: kcm.piperModelPath = text
        }
    }

    // Polly (cloud, opt-in): engine + region + an explicit billing note.
    RowLayout {
        Layout.fillWidth: true
        visible: kcm.ttsBackend === "polly"
        enabled: root.voicePresent
        QQC2.Label {
            text: "Polly engine"
            Layout.preferredWidth: 150
        }
        QQC2.ComboBox {
            id: pollyEngineCombo
            Layout.fillWidth: true
            textRole: "label"
            model: [
                { value: "neural", label: "Neural (cheaper, every region)" },
                { value: "generative", label: "Generative (most natural)" },
            ]
            currentIndex: Math.max(0, root.pollyEngineIndexById(kcm.pollyEngine))
            onActivated: kcm.pollyEngine = model[currentIndex].value
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: kcm.ttsBackend === "polly"
        enabled: root.voicePresent
        QQC2.Label {
            text: "Polly region"
            Layout.preferredWidth: 150
        }
        QQC2.TextField {
            id: pollyRegionField
            Layout.fillWidth: true
            placeholderText: "us-east-1"
            text: kcm.pollyRegion
            onEditingFinished: kcm.pollyRegion = text
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: kcm.ttsBackend === "polly"
        type: Kirigami.MessageType.Warning
        text: "Polly is a cloud service billed per character of synthesized "
              + "speech (your microphone audio is never sent — only the "
              + "assistant's reply text). Credentials come from the standard AWS "
              + "chain; for the systemd service set AWS_PROFILE/AWS_REGION in a "
              + "drop-in (~/.config/systemd/user/adele-voice.service.d/aws.conf)."
    }

    // Voice picker (live, D-Bus): reflects the active backend's voices.
    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: kcm.voiceServiceAvailable && (kcm.voiceList || []).length === 0
        opacity: 0.7
        text: "No TTS voices are available from the running backend yet. After "
              + "switching backend, restart the voice service to refresh this list."
    }

    RowLayout {
        Layout.fillWidth: true
        // NB: the disable gate lives on the ComboBox, not this row, so the
        // Refresh button stays usable when no voices are loaded yet (that's
        // exactly when the user needs to force a re-probe).
        QQC2.Label {
            text: "Voice"
            Layout.preferredWidth: 150
        }
        QQC2.ComboBox {
            id: voiceCombo
            Layout.fillWidth: true
            enabled: kcm.voiceServiceAvailable && (kcm.voiceList || []).length > 0
            textRole: "label"
            model: {
                const list = kcm.voiceList || []
                const out = []
                for (let i = 0; i < list.length; i++) {
                    out.push({ voice_id: list[i].voice_id, label: root.voiceLabel(list[i]) })
                }
                return out
            }
            currentIndex: Math.max(0, root.voiceIndexById(kcm.voiceCurrentId))
            onActivated: {
                const list = kcm.voiceList || []
                if (currentIndex >= 0 && currentIndex < list.length) {
                    // New voice resets speaker to default (-1); a multi-speaker
                    // voice then surfaces the speaker picker below.
                    kcm.setVoice(list[currentIndex].voice_id, -1)
                }
            }
        }
        // Manual fallback: re-probe the daemon (availability + voice list). The
        // C++ side also re-probes automatically when the daemon (re)appears on
        // the bus, but this lets the user force a refresh if the picker is empty.
        QQC2.Button {
            icon.name: "view-refresh"
            text: "Refresh"
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.text: "Re-check the voice service and reload its voices"
            onClicked: kcm.loadVoiceSettings()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        // Only meaningful for multi-speaker voices (e.g. VCTK).
        visible: kcm.voiceServiceAvailable && root.speakerCountFor(kcm.voiceCurrentId) > 1
        QQC2.Label {
            text: "Speaker"
            Layout.preferredWidth: 150
        }
        QQC2.ComboBox {
            id: speakerCombo
            Layout.fillWidth: true
            model: {
                const count = root.speakerCountFor(kcm.voiceCurrentId)
                const out = []
                for (let i = 0; i < count; i++) {
                    out.push("Speaker " + i)
                }
                return out
            }
            currentIndex: kcm.voiceCurrentSpeaker >= 0 ? kcm.voiceCurrentSpeaker : 0
            onActivated: kcm.setVoice(kcm.voiceCurrentId, currentIndex)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        QQC2.Button {
            text: "Apply now"
            icon.name: "dialog-ok-apply"
            // Apply config-file changes live (adele-kde#37): tries the daemon's
            // Reload D-Bus method (voice#52) and falls back to a service
            // restart. Enabled whenever the daemon is on the bus OR its unit is
            // installed (so a restart is possible).
            enabled: kcm.voiceServiceAvailable || kcm.voiceAutostart >= 0
            onClicked: kcm.applyVoiceChanges()
        }
        QQC2.Button {
            text: "Restart voice service"
            icon.name: "system-reboot"
            // Explicit full restart, for when a plain reload isn't enough.
            // Disabled when the unit isn't installed (nothing to restart).
            enabled: kcm.voiceAutostart >= 0
            onClicked: kcm.restartVoiceService()
        }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            text: "“Apply now” reloads the settings on this page (wake word, "
                  + "endpointing, devices, text-to-speech) without logging out."
        }
    }

    Kirigami.Separator { Layout.fillWidth: true }

    // --- Speech recognition + audio devices (config file) -------------------
    QQC2.Label {
        font.bold: true
        text: "Speech recognition & audio"
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "These are written to the voice config. Use “Apply now” above to "
              + "apply them without logging out."
    }

    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "STT language"
            Layout.preferredWidth: 150
        }
        QQC2.TextField {
            id: sttLanguageField
            Layout.fillWidth: true
            placeholderText: "en"
            text: kcm.sttLanguage
            onEditingFinished: kcm.sttLanguage = text
        }
    }

    // Whisper STT model selector (adele-kde#44): a catalog dropdown with on-disk
    // presence + in-KCM download, replacing the old free-form path field. The
    // free-form field is still reachable via the "Other / custom path…" option
    // (and is auto-selected when the configured path is hand-edited), so custom
    // paths round-trip losslessly.
    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Whisper model"
            Layout.preferredWidth: 150
        }
        QQC2.ComboBox {
            id: sttModelCombo
            Layout.fillWidth: true
            textRole: "label"
            // Disabled while a download is in flight so the selection can't move
            // out from under the transfer.
            enabled: root.voicePresent && !kcm.sttDownloadActive
            // Re-evaluated when the config or download state changes (presence
            // suffix + custom row depend on both).
            model: {
                // Touch the deps so the binding refreshes on change.
                void kcm.sttModelPath
                void kcm.sttDownloadActive
                void kcm.sttDownloadProgress
                return root.sttModelRows()
            }
            currentIndex: {
                void kcm.sttModelPath
                void kcm.sttDownloadActive
                return root.sttCurrentIndex()
            }
            onActivated: {
                const rows = root.sttModelRows()
                if (currentIndex < 0 || currentIndex >= rows.length) {
                    return
                }
                const row = rows[currentIndex]
                if (row.key === "other") {
                    // Reveal the free-form field without touching the stored
                    // path, so the user can type/keep an arbitrary path.
                    root.sttUseCustom = true
                    return
                }
                if (row.key === "custom-current") {
                    // Re-selecting the existing custom row: keep it, surface the
                    // free-form editor.
                    root.sttUseCustom = true
                    return
                }
                // Catalog model: write the ABSOLUTE path (daemon does not expand
                // ~), leave free-form mode. Restart-required; the apply/restart
                // flow at the top of the page picks it up.
                root.sttUseCustom = false
                kcm.sttModelPath = kcm.sttModelsDir() + "/" + row.file
            }
        }
        QQC2.Button {
            id: sttDownloadButton
            text: kcm.sttDownloadActive ? "Downloading…" : "Download"
            icon.name: "download"
            // Only meaningful for a catalog model that isn't on disk yet.
            visible: root.sttSelectedMissing || kcm.sttDownloadActive
            enabled: root.sttSelectedMissing && !kcm.sttDownloadActive
            onClicked: {
                const m = root.sttSelectedCatalogModel()
                if (m !== null) {
                    kcm.downloadSttModel(m.file, m.url)
                }
            }
        }
    }

    // Free-form custom path field (revealed via "Other / custom path…" or when
    // the configured path is hand-edited). Preserves arbitrary model paths.
    RowLayout {
        Layout.fillWidth: true
        visible: root.sttUseCustom
        enabled: root.voicePresent && !kcm.sttDownloadActive
        QQC2.Label {
            text: "Custom model path"
            Layout.preferredWidth: 150
        }
        QQC2.TextField {
            id: sttModelPathField
            Layout.fillWidth: true
            placeholderText: kcm.sttModelsDir() + "/ggml-distil-large-v3.bin"
            text: kcm.sttModelPath
            onEditingFinished: kcm.sttModelPath = text
        }
    }

    // Download progress (busy/percentage) while a model is being fetched.
    RowLayout {
        Layout.fillWidth: true
        visible: kcm.sttDownloadActive
        QQC2.Label {
            text: "Downloading " + kcm.sttDownloadingFile
            Layout.preferredWidth: 150
            elide: Text.ElideRight
        }
        QQC2.ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: 100
            // -1 = indeterminate (no Content-Length); show a busy bar.
            indeterminate: kcm.sttDownloadProgress < 0
            value: kcm.sttDownloadProgress < 0 ? 0 : kcm.sttDownloadProgress
        }
        QQC2.Button {
            text: "Cancel"
            icon.name: "dialog-cancel"
            onClicked: kcm.cancelSttModelDownload()
        }
    }

    // Inline warning when a selected catalog model isn't downloaded yet.
    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: root.sttSelectedMissing && !kcm.sttDownloadActive
        type: Kirigami.MessageType.Warning
        text: "This model isn’t downloaded yet. Click “Download” to fetch it "
              + "into " + kcm.sttModelsDir() + " before the voice service can "
              + "use it."
    }

    // Surface a failed download.
    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: kcm.sttDownloadError.length > 0 && !kcm.sttDownloadActive
        type: Kirigami.MessageType.Error
        text: "Model download failed: " + kcm.sttDownloadError
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: "Chooses the Whisper speech-recognition model. Larger models are "
              + "more accurate but slower and bigger to download. STT model "
              + "changes take effect after the voice service restarts — use "
              + "“Restart voice service” above."
    }

    // --- Audio devices (adele-kde#37) ---------------------------------------
    // Enumerated input/output selectors. The first row is always "Follow system
    // default" (recommended). A low-volume mic can silently sink wake-word
    // detection (voice#47), so we offer a level check + nudge below.
    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Input device"
            Layout.preferredWidth: 150
        }
        QQC2.ComboBox {
            id: inputDeviceCombo
            Layout.fillWidth: true
            textRole: "label"
            model: kcm.inputDeviceOptions
            currentIndex: root.deviceIndexByValue(kcm.inputDeviceOptions, kcm.inputDevice)
            onActivated: {
                const opts = kcm.inputDeviceOptions || []
                if (currentIndex >= 0 && currentIndex < opts.length) {
                    kcm.inputDevice = opts[currentIndex].value
                }
            }
        }
        QQC2.Button {
            icon.name: "view-refresh"
            text: "Refresh"
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.text: "Re-scan audio devices"
            onClicked: kcm.loadAudioDevices()
        }
    }

    // Mic level check + nudge (ties into voice#47): a too-quiet input can let
    // detection silently fail, so let the user verify there's signal.
    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Microphone level"
            Layout.preferredWidth: 150
        }
        QQC2.ProgressBar {
            id: micLevelBar
            Layout.fillWidth: true
            from: 0.0
            to: 1.0
            value: root.micLevel >= 0 ? root.micLevel : 0.0
        }
        QQC2.Button {
            text: "Test"
            icon.name: "audio-input-microphone"
            // measureInputLevel() is non-blocking (KDE-2 / #57, PR 5/5): it now
            // returns nothing and reports the result asynchronously via
            // kcm.inputLevelMeasured(level), handled by the Connections below.
            onClicked: kcm.measureInputLevel()
        }
    }

    // Receive the async microphone-level result (KDE-2 / #57, PR 5/5).
    Connections {
        target: kcm
        function onInputLevelMeasured(level) {
            root.micLevel = level
        }
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: root.micLevel >= 0
        // A peak under ~10% is almost always too quiet for reliable detection;
        // nudge the user to raise input gain rather than letting it fail
        // silently (voice#47).
        color: root.micLevel >= 0 && root.micLevel < 0.1
            ? Kirigami.Theme.negativeTextColor
            : Kirigami.Theme.textColor
        text: root.micLevel < 0
            ? ""
            : (root.micLevel < 0.1
                ? "That’s very quiet — say “Hey Adele” while testing. If the bar "
                  + "barely moves, raise your microphone’s input volume (e.g. in "
                  + "Audio settings) or the wake word may be missed."
                : "Good — the microphone is picking up audio.")
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: root.micLevel < 0
        opacity: 0.7
        text: "Press “Test”, then speak, to check your microphone level. "
              + "(Needs PulseAudio/PipeWire tools.)"
    }

    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Output device"
            Layout.preferredWidth: 150
        }
        QQC2.ComboBox {
            id: outputDeviceCombo
            Layout.fillWidth: true
            textRole: "label"
            model: kcm.outputDeviceOptions
            currentIndex: root.deviceIndexByValue(kcm.outputDeviceOptions, kcm.outputDevice)
            onActivated: {
                const opts = kcm.outputDeviceOptions || []
                if (currentIndex >= 0 && currentIndex < opts.length) {
                    kcm.outputDevice = opts[currentIndex].value
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        QQC2.Button {
            text: "Reset devices to system default"
            icon.name: "edit-undo"
            enabled: root.voicePresent
            onClicked: kcm.resetDeviceDefaults()
        }
        Item { Layout.fillWidth: true }
    }

    Kirigami.Separator { Layout.fillWidth: true }

    // --- Reset everything (adele-kde#37) ------------------------------------
    RowLayout {
        Layout.fillWidth: true
        QQC2.Button {
            text: "Reset all voice tuning to defaults"
            icon.name: "edit-undo"
            enabled: root.voicePresent
            onClicked: kcm.resetVoiceTuningDefaults()
        }
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            text: "Restores wake-word sensitivity, endpointing, and audio "
                  + "devices to their recommended defaults. Use “Apply now” "
                  + "above to apply."
        }
    }

    Kirigami.Separator { Layout.fillWidth: true }

    // --- Autostart (systemd user unit) --------------------------------------
    QQC2.Label {
        font.bold: true
        text: "Startup"
    }

    QQC2.CheckBox {
        id: autostartCheck
        text: "Start the voice service at login"
        enabled: kcm.voiceAutostart >= 0
        checked: kcm.voiceAutostart === 1
        onToggled: kcm.setVoiceAutostart(checked)
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        opacity: 0.7
        text: kcm.voiceAutostart < 0
            ? "The voice service's systemd unit isn't installed, so autostart "
              + "can't be configured here."
            : "Enables or disables the adele-voice systemd user unit. It also "
              + "starts on demand whenever an app calls the voice service."
    }

    Item { Layout.fillHeight: true }

    // Keep editable text fields + sliders/spin boxes in sync when the KCM
    // reloads config from disk (including after a Reset). The ComboBoxes
    // (TTS backend/engine, audio devices) re-derive their index from kcm.* via
    // their currentIndex bindings, so only the free-form controls need explicit
    // sync; we guard live drag/edit with `.pressed`/focus where it matters.
    Connections {
        target: kcm
        // The "Enable Hey Adele" checkbox writes kcm.voiceEnabled on toggle,
        // which breaks its `checked: kcm.voiceEnabled` binding. After that the
        // box no longer tracks the live daemon state, so an external start/stop
        // — or the m_voiceWatcher re-probe after "Restart voice service" — would
        // leave it stale. Re-assert it here on every voiceChanged (KDE-10).
        function onVoiceChanged() {
            if (enableHeyAdeleCheck.checked !== kcm.voiceEnabled) {
                enableHeyAdeleCheck.checked = kcm.voiceEnabled
            }
        }
        function onVoiceConfigChanged() {
            if (sttLanguageField.text !== kcm.sttLanguage) {
                sttLanguageField.text = kcm.sttLanguage
            }
            if (sttModelPathField.text !== kcm.sttModelPath) {
                sttModelPathField.text = kcm.sttModelPath
            }
            // If a reload (or external edit) lands a custom/hand-edited path,
            // reveal the free-form field so it round-trips; a catalog path leaves
            // it hidden unless the user explicitly chose "Other / custom path…".
            if (WhisperModels.isCustomPath(kcm.sttModelPath)) {
                root.sttUseCustom = true
            }
            if (kokoroLangField.text !== kcm.kokoroLang) {
                kokoroLangField.text = kcm.kokoroLang
            }
            if (piperModelPathField.text !== kcm.piperModelPath) {
                piperModelPathField.text = kcm.piperModelPath
            }
            if (pollyRegionField.text !== kcm.pollyRegion) {
                pollyRegionField.text = kcm.pollyRegion
            }
            if (!sensitivitySlider.pressed && sensitivitySlider.value !== kcm.wakeSensitivity) {
                sensitivitySlider.value = kcm.wakeSensitivity
            }
            if (!speechThresholdSlider.pressed && speechThresholdSlider.value !== kcm.vadSpeechThreshold) {
                speechThresholdSlider.value = kcm.vadSpeechThreshold
            }
            if (silenceDurationSpin.value !== kcm.vadSilenceDurationMs) {
                silenceDurationSpin.value = kcm.vadSilenceDurationMs
            }
            if (followupTimeoutSpin.value !== kcm.followupTimeoutMs) {
                followupTimeoutSpin.value = kcm.followupTimeoutMs
            }
            wakeEagerCheck.checked = kcm.wakeEager
        }
    }
}
