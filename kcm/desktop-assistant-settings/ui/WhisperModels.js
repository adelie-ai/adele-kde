.pragma library

// Curated Whisper STT model catalog for the Voice page's model selector
// (adele-kde#44). Factored out of VoicePage.qml — like VoiceBackends.js and
// AudioDevices.js — so the file<->row mapping and the custom-path logic can be
// unit-tested without instantiating the page (which needs the C++ `kcm` context
// object and so is only compile-probed headless; see tests/qml/tst_WhisperModels.qml).
//
// This array is the SINGLE SOURCE OF TRUTH for the dropdown: VoicePage.qml
// builds the ComboBox `model` from MODELS, selects the current row via
// modelIndexByFile(basename(kcm.sttModelPath)), and on selection writes back the
// absolute path (kcm.sttModelsDir() + "/" + MODELS[i].file). Filenames follow
// the whisper.cpp / distil-whisper ggml conventions and are also the basenames
// the daemon resolves under $XDG_DATA_HOME/adele-voice/models.
//
// Fields per entry:
//   * file   — ggml model basename (what lands in the models dir + config.toml)
//   * label  — human-readable ComboBox text
//   * url    — direct download URL (HuggingFace `resolve/main`)
//   * sizeMb — approximate on-disk size, MB (display/sanity only)
//   * note   — short tradeoff blurb
var MODELS = [
    {
        file: "ggml-distil-large-v3.bin",
        label: "Distil Large v3 — most accurate (~1.5 GB)",
        url: "https://huggingface.co/distil-whisper/distil-large-v3-ggml/resolve/main/ggml-distil-large-v3.bin",
        sizeMb: 1500,
        note: "Best accuracy; largest download and slowest on CPU.",
    },
    {
        file: "ggml-small.en.bin",
        label: "Small (English) — balanced (~488 MB)",
        url: "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin",
        sizeMb: 488,
        note: "Good accuracy/speed balance for English.",
    },
    {
        file: "ggml-base.en.bin",
        label: "Base (English) — fastest (~148 MB)",
        url: "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin",
        sizeMb: 148,
        note: "Fastest; lower accuracy, best for low-end machines.",
    },
    {
        file: "ggml-medium.en.bin",
        label: "Medium (English) — accurate, slower (~1.5 GB)",
        url: "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.en.bin",
        sizeMb: 1500,
        note: "More accurate than Small; heavier on CPU.",
    },
]

// Basename of a "path/to/file" (or a bare filename). Splits on both '/' and '\'
// so a hand-edited Windows-style or absolute POSIX path still reduces to the
// filename we match against the catalog. Empty/null/undefined -> "".
function basename(path) {
    var s = String(path === null || path === undefined ? "" : path)
    if (s.length === 0) {
        return ""
    }
    var slash = Math.max(s.lastIndexOf("/"), s.lastIndexOf("\\"))
    return slash < 0 ? s : s.substring(slash + 1)
}

// Row of a model in MODELS keyed by its `file` basename. Accepts either a bare
// filename or a full path (the basename is taken). Unknown/empty/null -> 0
// (never -1), so the ComboBox always lands on a valid catalog row.
function modelIndexByFile(file) {
    var name = basename(file)
    for (var i = 0; i < MODELS.length; i++) {
        if (MODELS[i].file === name) {
            return i
        }
    }
    return 0
}

// True when `path` is a non-empty STT model path whose basename is NOT in the
// catalog — i.e. a hand-edited / custom model the dropdown must preserve rather
// than clobber (shown as a "Custom: <path>" row). An empty path is not custom
// (it means "daemon default"), and a catalog model is not custom.
function isCustomPath(path) {
    var s = String(path === null || path === undefined ? "" : path).trim()
    if (s.length === 0) {
        return false
    }
    var name = basename(s)
    for (var i = 0; i < MODELS.length; i++) {
        if (MODELS[i].file === name) {
            return false
        }
    }
    return true
}
