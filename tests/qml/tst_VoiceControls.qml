import QtQuick
import QtTest 1.0

import "../../shared/chat-module/ui" as Chat

// Voice-control unit tests for ChatView (adele-kde#29).
//
// ChatView talks to the voice daemon through a one-shot Python helper, so we
// can't exercise the live D-Bus round-trip here (no daemon on the CI bus).
// What we CAN pin without a backend are the pure, backend-independent pieces
// of the voice UI contract:
//
//   * graceful degradation — with no voice service the view loads, stays
//     `voiceAvailable: false`, and the mutating helpers are inert no-ops
//     (they must never throw, which would break the whole widget),
//   * the voice-list lookup helpers (`voiceIndexById`, `voiceSpeakerCount`)
//     that drive the switcher + multi-speaker combo,
//   * the derived state label/icon mapping for the mic button.
//
// The live "toggle flips wake on/off", "record starts a turn", and "switcher
// changes the active voice" behaviours are covered by the Python helper tests
// (test_voice_commands.py) and need a running daemon for end-to-end proof.
TestCase {
    id: testCase
    name: "VoiceControls"
    when: windowShown
    width: 520
    height: 620
    visible: true

    Component {
        id: chatViewComponent
        Chat.ChatView {
            anchors.fill: parent
        }
    }

    function test_defaults_to_unavailable_and_idle() {
        var view = createTemporaryObject(chatViewComponent, testCase)
        verify(view !== null, "ChatView instantiated")
        // With no voice daemon on the bus the controls must be dormant.
        compare(view.voiceAvailable, false)
        compare(view.voiceState, "Idle")
        compare(view.voiceEnabled, false)
        // The empty-availability label is blank so nothing dangles in the UI.
        compare(view.voiceStateLabel, "")
    }

    function test_mutating_helpers_are_inert_when_unavailable() {
        var view = createTemporaryObject(chatViewComponent, testCase)
        verify(view !== null, "ChatView instantiated")
        // None of these should throw or flip state while the service is down;
        // a thrown JS error here would abort the whole widget.
        view.voicePushToTalk()
        view.voiceStopSpeaking()
        view.setVoiceEnabled(true)
        view.selectVoice("en_US-amy-medium", -1)
        compare(view.voiceAvailable, false)
        compare(view.voiceEnabled, false)
        compare(view.voiceState, "Idle")
    }

    function test_voice_index_and_speaker_count_lookup() {
        var view = createTemporaryObject(chatViewComponent, testCase)
        verify(view !== null, "ChatView instantiated")
        view.voiceChoices = [
            { voice_id: "amy", display_name: "Amy", language: "en_US", num_speakers: 1, label: "Amy (en_US)" },
            { voice_id: "vctk", display_name: "VCTK", language: "en_GB", num_speakers: 109, label: "VCTK (en_GB)" },
        ]
        compare(view.voiceIndexById("vctk"), 1)
        compare(view.voiceIndexById("missing"), -1)
        // Single-speaker voice reports 1 (so the speaker combo hides);
        // multi-speaker reports its real count.
        compare(view.voiceSpeakerCount("amy"), 1)
        compare(view.voiceSpeakerCount("vctk"), 109)
        // Unknown id falls back to 1 rather than 0/NaN.
        compare(view.voiceSpeakerCount("missing"), 1)
    }

    function test_state_icon_tracks_pipeline_state() {
        var view = createTemporaryObject(chatViewComponent, testCase)
        verify(view !== null, "ChatView instantiated")
        view.voiceState = "Listening"
        compare(view.voiceStateIcon, "audio-input-microphone")
        view.voiceState = "Speaking"
        compare(view.voiceStateIcon, "audio-volume-high")
        view.voiceState = "Idle"
        compare(view.voiceStateIcon, "audio-input-microphone-muted")
    }

    function test_state_label_when_available() {
        var view = createTemporaryObject(chatViewComponent, testCase)
        verify(view !== null, "ChatView instantiated")
        view.voiceAvailable = true
        view.voiceState = "Processing"
        compare(view.voiceStateLabel, "Thinking…")
        view.voiceState = "Listening"
        compare(view.voiceStateLabel, "Listening…")
    }

    function test_build_voice_label_includes_language() {
        var view = createTemporaryObject(chatViewComponent, testCase)
        verify(view !== null, "ChatView instantiated")
        compare(view.buildVoiceLabel({ display_name: "Amy", language: "en_US" }), "Amy (en_US)")
        // No language -> just the name, no empty parens.
        compare(view.buildVoiceLabel({ display_name: "Amy", language: "" }), "Amy")
    }

    function test_push_to_talk_dictates_into_active_conversation() {
        // voice#24: with a conversation open, the mic button dictates into THAT
        // conversation — the helper args carry --conversation-id <id> so the
        // daemon routes the prompt + spoken reply to the chat in view.
        var view = createTemporaryObject(chatViewComponent, testCase)
        verify(view !== null, "ChatView instantiated")
        var args = view.pushToTalkHelperArgs("conv-abc")
        verify(args.indexOf("voice-push-to-talk") !== -1, "calls the PTT helper")
        verify(args.indexOf("--conversation-id") !== -1, "passes the id flag")
        verify(args.indexOf("conv-abc") !== -1, "includes the active conversation id")
    }

    function test_push_to_talk_without_conversation_uses_own_session() {
        // No conversation open -> plain voice-push-to-talk (the daemon's own
        // session); no id flag is appended.
        var view = createTemporaryObject(chatViewComponent, testCase)
        verify(view !== null, "ChatView instantiated")
        compare(view.pushToTalkHelperArgs(""), "voice-push-to-talk")
    }
}
