.pragma library

// Pure mapping helpers for the Voice page's TTS backend / Polly engine pickers
// (adele-kde#33). Factored out of VoicePage.qml so the index<->token mapping can
// be unit-tested without instantiating the page (which needs the C++ `kcm`
// context object and so is only compile-probed headless).
//
// The order arrays MUST stay in lockstep with the ComboBox `model` order in
// VoicePage.qml — the page binds `currentIndex` to these and writes back
// `model[currentIndex].value`, so a mismatch would silently select the wrong
// backend/engine.

// tts.backend tokens, in ComboBox order. Kokoro is the daemon default.
var BACKENDS = ["kokoro", "piper", "polly"]

// polly_engine tokens the GUI offers, in ComboBox order. The daemon also
// accepts "long-form"/"standard"; those map to index 0 (neural) here.
var POLLY_ENGINES = ["neural", "generative"]

// Index of a backend token in the picker; unknown/empty -> 0 (never -1), so
// the ComboBox always lands on a valid row (the default backend).
function backendIndexById(id) {
    var idx = BACKENDS.indexOf(String(id))
    return idx < 0 ? 0 : idx
}

// Index of a Polly engine token; unknown/empty/long-form/standard -> 0 (neural).
function pollyEngineIndexById(id) {
    var idx = POLLY_ENGINES.indexOf(String(id))
    return idx < 0 ? 0 : idx
}
