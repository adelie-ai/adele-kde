/*
 * Personality settings page (adele-kde#42).
 *
 * Seven discrete 0..4 sliders that set the assistant's global personality
 * disposition: Professionalism, Warmth, Directness, Enthusiasm, Humor,
 * Sarcasm, Pretentiousness. Each step is shown as a word (Never..Always),
 * not a number.
 *
 * These persist on the orchestrator's aggregate config (D-Bus interface
 * org.desktopAssistant.Settings — the SAME bus the rest of this KCM uses, NOT
 * the voice daemon's). They hot-apply: like the Voice tab's live controls there
 * is no Apply button — each slider's onMoved writes immediately via the C++
 * setter (kcm.personality* -> SetConfig with set_personality_<trait>=true).
 *
 * As with the other KCM pages this file is declarative: it binds to kcm.*
 * properties; all D-Bus access happens C++-side. The trait table + int<->word
 * mapping live in Personality.js so they can be unit-tested without the C++
 * `kcm` context (see tests/qml/tst_PersonalityPage.qml).
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

import "Personality.js" as Personality

ColumnLayout {
    id: root
    spacing: 12

    // The 0..4 -> word mapping the row labels use. Thin wrapper over the pure
    // helper in Personality.js (kept so the call site stays binding-friendly).
    function wordForValue(value) {
        return Personality.wordForValue(value)
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: true
        type: Kirigami.MessageType.Information
        text: "These set the assistant's starting personality — an initial "
              + "disposition it adapts from as it gets to know you, not a hard "
              + "rule. Each trait runs from “Never” to “Always”. Changes apply "
              + "immediately and shape the next thing the assistant says."
    }

    // --- Professionalism ----------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        QQC2.Label {
            text: "Professionalism"
            Layout.preferredWidth: 150
        }
        QQC2.Slider {
            id: professionalismSlider
            Layout.fillWidth: true
            from: 0
            to: 4
            stepSize: 1
            snapMode: QQC2.Slider.SnapAlways
            value: kcm.personalityProfessionalism
            onMoved: kcm.personalityProfessionalism = value
        }
        QQC2.Label {
            Layout.preferredWidth: 90
            text: root.wordForValue(professionalismSlider.value)
        }
    }

    // --- Warmth -------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        QQC2.Label {
            text: "Warmth"
            Layout.preferredWidth: 150
        }
        QQC2.Slider {
            id: warmthSlider
            Layout.fillWidth: true
            from: 0
            to: 4
            stepSize: 1
            snapMode: QQC2.Slider.SnapAlways
            value: kcm.personalityWarmth
            onMoved: kcm.personalityWarmth = value
        }
        QQC2.Label {
            Layout.preferredWidth: 90
            text: root.wordForValue(warmthSlider.value)
        }
    }

    // --- Directness ---------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        QQC2.Label {
            text: "Directness"
            Layout.preferredWidth: 150
        }
        QQC2.Slider {
            id: directnessSlider
            Layout.fillWidth: true
            from: 0
            to: 4
            stepSize: 1
            snapMode: QQC2.Slider.SnapAlways
            value: kcm.personalityDirectness
            onMoved: kcm.personalityDirectness = value
        }
        QQC2.Label {
            Layout.preferredWidth: 90
            text: root.wordForValue(directnessSlider.value)
        }
    }

    // --- Enthusiasm ---------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        QQC2.Label {
            text: "Enthusiasm"
            Layout.preferredWidth: 150
        }
        QQC2.Slider {
            id: enthusiasmSlider
            Layout.fillWidth: true
            from: 0
            to: 4
            stepSize: 1
            snapMode: QQC2.Slider.SnapAlways
            value: kcm.personalityEnthusiasm
            onMoved: kcm.personalityEnthusiasm = value
        }
        QQC2.Label {
            Layout.preferredWidth: 90
            text: root.wordForValue(enthusiasmSlider.value)
        }
    }

    // --- Humor --------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        QQC2.Label {
            text: "Humor"
            Layout.preferredWidth: 150
        }
        QQC2.Slider {
            id: humorSlider
            Layout.fillWidth: true
            from: 0
            to: 4
            stepSize: 1
            snapMode: QQC2.Slider.SnapAlways
            value: kcm.personalityHumor
            onMoved: kcm.personalityHumor = value
        }
        QQC2.Label {
            Layout.preferredWidth: 90
            text: root.wordForValue(humorSlider.value)
        }
    }

    // --- Sarcasm ------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        QQC2.Label {
            text: "Sarcasm"
            Layout.preferredWidth: 150
        }
        QQC2.Slider {
            id: sarcasmSlider
            Layout.fillWidth: true
            from: 0
            to: 4
            stepSize: 1
            snapMode: QQC2.Slider.SnapAlways
            value: kcm.personalitySarcasm
            onMoved: kcm.personalitySarcasm = value
        }
        QQC2.Label {
            Layout.preferredWidth: 90
            text: root.wordForValue(sarcasmSlider.value)
        }
    }

    // --- Pretentiousness ----------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        QQC2.Label {
            text: "Pretentiousness"
            Layout.preferredWidth: 150
        }
        QQC2.Slider {
            id: pretentiousnessSlider
            Layout.fillWidth: true
            from: 0
            to: 4
            stepSize: 1
            snapMode: QQC2.Slider.SnapAlways
            value: kcm.personalityPretentiousness
            onMoved: kcm.personalityPretentiousness = value
        }
        QQC2.Label {
            Layout.preferredWidth: 90
            text: root.wordForValue(pretentiousnessSlider.value)
        }
    }

    Item { Layout.fillHeight: true }

    // Keep the sliders in sync when the KCM reloads config from the daemon
    // (load() / after a SetConfig round-trip), but never fight a live drag:
    // guard each write with `.pressed`. The value labels re-derive from the
    // slider value binding, so only the sliders need explicit sync.
    Connections {
        target: kcm
        function onPersonalityChanged() {
            if (!professionalismSlider.pressed && professionalismSlider.value !== kcm.personalityProfessionalism) {
                professionalismSlider.value = kcm.personalityProfessionalism
            }
            if (!warmthSlider.pressed && warmthSlider.value !== kcm.personalityWarmth) {
                warmthSlider.value = kcm.personalityWarmth
            }
            if (!directnessSlider.pressed && directnessSlider.value !== kcm.personalityDirectness) {
                directnessSlider.value = kcm.personalityDirectness
            }
            if (!enthusiasmSlider.pressed && enthusiasmSlider.value !== kcm.personalityEnthusiasm) {
                enthusiasmSlider.value = kcm.personalityEnthusiasm
            }
            if (!humorSlider.pressed && humorSlider.value !== kcm.personalityHumor) {
                humorSlider.value = kcm.personalityHumor
            }
            if (!sarcasmSlider.pressed && sarcasmSlider.value !== kcm.personalitySarcasm) {
                sarcasmSlider.value = kcm.personalitySarcasm
            }
            if (!pretentiousnessSlider.pressed && pretentiousnessSlider.value !== kcm.personalityPretentiousness) {
                pretentiousnessSlider.value = kcm.personalityPretentiousness
            }
        }
    }
}
