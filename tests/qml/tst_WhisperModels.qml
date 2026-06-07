import QtQuick
import QtTest 1.0

import "../../kcm/desktop-assistant-settings/ui/WhisperModels.js" as WhisperModels

// Unit tests for the Voice page's Whisper STT model selector (adele-kde#44).
//
// VoicePage.qml builds the model ComboBox from WhisperModels.MODELS, selects the
// current row via modelIndexByFile(basename(kcm.sttModelPath)), and decides
// whether to reveal the free-form custom path field via isCustomPath(). The page
// needs the C++ `kcm` context object and can't be instantiated headless (it's
// compile-probed in tst_QmlComponentsLoad), but the catalog mapping + custom
// detection are pure logic, so we pin them here — the same approach as
// tst_VoiceBackends / tst_AudioDevices.
//
// The contract under test:
//   * every catalog model maps to its expected row, and the catalog order is
//     stable (distil-large-v3, small.en, base.en, medium.en),
//   * a bare filename and a full path both resolve to the catalog row,
//   * unknown/empty/null model files fall back to row 0 (never -1) so the
//     ComboBox never renders blank,
//   * isCustomPath() correctly distinguishes hand-edited paths (preserve) from
//     catalog models and the empty/default case.
TestCase {
    id: testCase
    name: "WhisperModels"

    // --- catalog shape ------------------------------------------------------

    function test_catalog_order_is_stable() {
        var files = WhisperModels.MODELS.map(function (m) { return m.file })
        compare(files, [
            "ggml-distil-large-v3.bin",
            "ggml-small.en.bin",
            "ggml-base.en.bin",
            "ggml-medium.en.bin",
        ])
    }

    function test_catalog_entries_have_required_fields() {
        // Every entry must carry the fields VoicePage + the downloader rely on.
        for (var i = 0; i < WhisperModels.MODELS.length; i++) {
            var m = WhisperModels.MODELS[i]
            verify(m.file.length > 0, "entry " + i + " missing file")
            verify(m.label.length > 0, "entry " + i + " missing label")
            verify(m.url.indexOf("https://") === 0,
                   "entry " + i + " url must be https: " + m.url)
            verify(m.sizeMb > 0, "entry " + i + " missing sizeMb")
            verify(m.note.length > 0, "entry " + i + " missing note")
        }
    }

    // --- basename -----------------------------------------------------------

    function test_basename_of_bare_filename() {
        compare(WhisperModels.basename("ggml-base.en.bin"), "ggml-base.en.bin")
    }

    function test_basename_of_absolute_path() {
        compare(WhisperModels.basename("/home/u/.local/share/adele-voice/models/ggml-small.en.bin"),
                "ggml-small.en.bin")
    }

    function test_basename_of_empty_and_null() {
        compare(WhisperModels.basename(""), "")
        compare(WhisperModels.basename(null), "")
        compare(WhisperModels.basename(undefined), "")
    }

    // --- modelIndexByFile (happy path) --------------------------------------

    function test_index_distil_is_zero() {
        compare(WhisperModels.modelIndexByFile("ggml-distil-large-v3.bin"), 0)
    }

    function test_index_small_is_one() {
        compare(WhisperModels.modelIndexByFile("ggml-small.en.bin"), 1)
    }

    function test_index_base_is_two() {
        compare(WhisperModels.modelIndexByFile("ggml-base.en.bin"), 2)
    }

    function test_index_medium_is_three() {
        compare(WhisperModels.modelIndexByFile("ggml-medium.en.bin"), 3)
    }

    function test_index_accepts_full_path() {
        // The configured path is absolute; modelIndexByFile takes the basename.
        compare(WhisperModels.modelIndexByFile(
            "/home/u/.local/share/adele-voice/models/ggml-small.en.bin"), 1)
    }

    // --- modelIndexByFile (unknown -> 0) ------------------------------------

    function test_index_unknown_falls_back_to_zero() {
        // A hand-edited/custom model file must not select -1 (blank combo); the
        // catalog mapping falls back to row 0 (the page renders it as a separate
        // "Custom:" row, but the pure mapping never returns -1).
        compare(WhisperModels.modelIndexByFile("ggml-tiny.bin"), 0)
        compare(WhisperModels.modelIndexByFile("/opt/models/my-custom.bin"), 0)
    }

    function test_index_empty_and_null_safe() {
        compare(WhisperModels.modelIndexByFile(""), 0)
        compare(WhisperModels.modelIndexByFile(null), 0)
        compare(WhisperModels.modelIndexByFile(undefined), 0)
    }

    // --- isCustomPath (custom-path preservation logic) ----------------------

    function test_custom_path_detected() {
        // A non-empty path whose basename isn't in the catalog is custom and
        // must be preserved (revealed in the free-form field, never clobbered).
        verify(WhisperModels.isCustomPath("/opt/models/my-finetune.bin"))
        verify(WhisperModels.isCustomPath("ggml-large-v3.bin")) // not in catalog
    }

    function test_catalog_path_is_not_custom() {
        // A catalog model (bare or full path) is NOT custom.
        verify(!WhisperModels.isCustomPath("ggml-small.en.bin"))
        verify(!WhisperModels.isCustomPath(
            "/home/u/.local/share/adele-voice/models/ggml-base.en.bin"))
    }

    function test_empty_path_is_not_custom() {
        // Empty == "use the daemon default", which is not a custom path.
        verify(!WhisperModels.isCustomPath(""))
        verify(!WhisperModels.isCustomPath("   "))
        verify(!WhisperModels.isCustomPath(null))
        verify(!WhisperModels.isCustomPath(undefined))
    }
}
