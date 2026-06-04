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

ColumnLayout {
    id: root
    spacing: 12

    // The voice feature is "present" if either the live daemon is on the bus
    // OR its config/unit exists locally (so you can pre-configure before first
    // launch). When neither is true the page shows an explainer and disables.
    readonly property bool voicePresent: kcm.voiceServiceAvailable || kcm.voiceAutostart >= 0

    function voiceIndexById(id) {
        const list = kcm.voiceList || []
        for (let i = 0; i < list.length; i++) {
            if (list[i].voice_id === id) {
                return i
            }
        }
        return -1
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

    Component.onCompleted: kcm.loadVoiceSettings()

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
            stepSize: 0.05
            value: kcm.wakeSensitivity
            onMoved: kcm.wakeSensitivity = value
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
        text: "Higher is more sensitive (more triggers, more false positives). "
              + "Applied on the next service restart."
    }

    Kirigami.Separator { Layout.fillWidth: true }

    // --- Voice (TTS, live D-Bus) --------------------------------------------
    QQC2.Label {
        font.bold: true
        text: "Spoken voice"
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: kcm.voiceServiceAvailable && (kcm.voiceList || []).length === 0
        opacity: 0.7
        text: "No TTS voices are installed. Add a Piper voice model to the "
              + "voice daemon's model directory."
    }

    RowLayout {
        Layout.fillWidth: true
        enabled: kcm.voiceServiceAvailable && (kcm.voiceList || []).length > 0
        QQC2.Label {
            text: "Voice"
            Layout.preferredWidth: 150
        }
        QQC2.ComboBox {
            id: voiceCombo
            Layout.fillWidth: true
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
        text: "These are written to the voice config and take effect the next "
              + "time the voice service starts."
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

    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Input device"
            Layout.preferredWidth: 150
        }
        QQC2.TextField {
            id: inputDeviceField
            Layout.fillWidth: true
            placeholderText: "default"
            text: kcm.inputDevice
            onEditingFinished: kcm.inputDevice = text
        }
    }

    RowLayout {
        Layout.fillWidth: true
        enabled: root.voicePresent
        QQC2.Label {
            text: "Output device"
            Layout.preferredWidth: 150
        }
        QQC2.TextField {
            id: outputDeviceField
            Layout.fillWidth: true
            placeholderText: "default"
            text: kcm.outputDevice
            onEditingFinished: kcm.outputDevice = text
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

    // Keep editable text fields in sync when the KCM reloads config from disk.
    Connections {
        target: kcm
        function onVoiceConfigChanged() {
            if (sttLanguageField.text !== kcm.sttLanguage) {
                sttLanguageField.text = kcm.sttLanguage
            }
            if (inputDeviceField.text !== kcm.inputDevice) {
                inputDeviceField.text = kcm.inputDevice
            }
            if (outputDeviceField.text !== kcm.outputDevice) {
                outputDeviceField.text = kcm.outputDevice
            }
            if (!sensitivitySlider.pressed && sensitivitySlider.value !== kcm.wakeSensitivity) {
                sensitivitySlider.value = kcm.wakeSensitivity
            }
        }
    }
}
