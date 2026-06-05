import QtQuick
import QtTest 1.0

import "../../kcm/desktop-assistant-settings/ui/VoiceBackends.js" as VoiceBackends

// Unit tests for the Voice page's TTS backend / Polly engine pickers
// (adele-kde#33).
//
// VoicePage.qml binds the backend and engine ComboBoxes' `currentIndex` to
// VoiceBackends.backendIndexById(kcm.ttsBackend) /
// .pollyEngineIndexById(kcm.pollyEngine), and on selection writes back
// `model[currentIndex].value`. The page itself needs the C++ `kcm` context
// object and can't be instantiated headless (it's compile-probed in
// tst_QmlComponentsLoad), but this index<->token mapping is pure logic, so we
// pin it directly — the same approach as tst_LinkSafety.
//
// The contract under test:
//   * every backend/engine token maps to its expected ComboBox row, and that
//     row's order matches VoicePage's `model` array (kokoro/piper/polly and
//     neural/generative),
//   * unknown/empty/null tokens fall back to row 0 (the default) rather than
//     -1, so the ComboBox never renders blank or selects nothing.
TestCase {
    id: testCase
    name: "VoiceBackends"

    // --- TTS backend mapping ------------------------------------------------

    function test_backend_index_kokoro_is_zero() {
        // Kokoro is the daemon default and must be the first row.
        compare(VoiceBackends.backendIndexById("kokoro"), 0)
    }

    function test_backend_index_piper_is_one() {
        compare(VoiceBackends.backendIndexById("piper"), 1)
    }

    function test_backend_index_polly_is_two() {
        compare(VoiceBackends.backendIndexById("polly"), 2)
    }

    function test_backend_index_unknown_falls_back_to_default() {
        // A stray/legacy token must not select -1 (blank combo); fall back to
        // the default backend (row 0).
        compare(VoiceBackends.backendIndexById("espeak"), 0)
    }

    function test_backend_index_empty_falls_back_to_default() {
        compare(VoiceBackends.backendIndexById(""), 0)
    }

    function test_backend_index_null_safe() {
        // Defensive: null/undefined must return 0, not throw.
        compare(VoiceBackends.backendIndexById(null), 0)
        compare(VoiceBackends.backendIndexById(undefined), 0)
    }

    function test_backend_order_matches_tokens() {
        // The BACKENDS order array is the source of truth the VoicePage model
        // mirrors; pin it so the two can't silently drift.
        compare(VoiceBackends.BACKENDS, ["kokoro", "piper", "polly"])
    }

    // --- Polly engine mapping ----------------------------------------------

    function test_polly_engine_index_neural_is_zero() {
        compare(VoiceBackends.pollyEngineIndexById("neural"), 0)
    }

    function test_polly_engine_index_generative_is_one() {
        compare(VoiceBackends.pollyEngineIndexById("generative"), 1)
    }

    function test_polly_engine_index_unknown_falls_back_to_neural() {
        // The daemon also accepts long-form/standard, which the GUI doesn't
        // offer; those map to neural (row 0) rather than -1.
        compare(VoiceBackends.pollyEngineIndexById("long-form"), 0)
        compare(VoiceBackends.pollyEngineIndexById("standard"), 0)
    }

    function test_polly_engine_index_empty_and_null_safe() {
        compare(VoiceBackends.pollyEngineIndexById(""), 0)
        compare(VoiceBackends.pollyEngineIndexById(null), 0)
        compare(VoiceBackends.pollyEngineIndexById(undefined), 0)
    }

    function test_polly_engine_order_matches_tokens() {
        compare(VoiceBackends.POLLY_ENGINES, ["neural", "generative"])
    }
}
