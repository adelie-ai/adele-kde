.pragma library

// Pure logic for the Personality page (adele-kde#42). Factored out of
// PersonalityPage.qml so the int<->word mapping and the trait table can be
// unit-tested without instantiating the page (which needs the C++ `kcm`
// context object and so is only compile-probed headless) — the same approach
// as VoiceBackends.js / AudioDevices.js.
//
// The seven personality traits are a discrete 0..4 dial each. They live ONLY on
// the daemon's aggregate GetConfig/SetConfig (org.desktopAssistant.Settings) as
// per-trait u32 fields; the KCM C++ side reads/writes them there. This file
// owns the presentation-side contract: the word scale and the trait metadata
// (display order, the kcm.<prop> bound by each row, and the built-in default
// used when the daemon can't be reached).

// The 5 discrete steps, in value order. The slider is from:0 to:4 stepSize:1
// and shows WORDS[value] next to it (never the bare number).
var WORDS = ["Never", "Rarely", "Sometimes", "Often", "Always"]

// Trait table, in display order (issue #42). `prop` is the kcm Q_PROPERTY each
// slider binds to / writes back; `default` is the built-in fallback applied when
// GetConfig fails (daemon down) — Professionalism=4, Warmth=3, Directness=3,
// Enthusiasm=2, Humor=2, Sarcasm=1, Pretentiousness=1.
var TRAITS = [
    { prop: "personalityProfessionalism", label: "Professionalism", default: 4 },
    { prop: "personalityWarmth",          label: "Warmth",          default: 3 },
    { prop: "personalityDirectness",      label: "Directness",      default: 3 },
    { prop: "personalityEnthusiasm",      label: "Enthusiasm",      default: 2 },
    { prop: "personalityHumor",           label: "Humor",           default: 2 },
    { prop: "personalitySarcasm",         label: "Sarcasm",         default: 1 },
    { prop: "personalityPretentiousness", label: "Pretentiousness", default: 1 },
]

// Map a 0..4 trait value to its word. Out-of-range / non-integer / null input is
// clamped to the nearest valid step so the label never renders blank or
// "undefined" (the daemon also rejects out-of-range writes with InvalidArgs, so
// we keep the UI inside 0..4 too).
function wordForValue(value) {
    return WORDS[clampStep(value)]
}

// Clamp an arbitrary input to a valid 0..4 integer step.
function clampStep(value) {
    var n = Math.round(Number(value))
    if (isNaN(n) || n < 0) {
        return 0
    }
    if (n > WORDS.length - 1) {
        return WORDS.length - 1
    }
    return n
}
