import QtQuick
import QtTest 1.0

import "../../kcm/desktop-assistant-settings/ui/Personality.js" as Personality

// Unit tests for the Personality page's pure logic (adele-kde#42).
//
// PersonalityPage.qml has 7 discrete 0..4 sliders (Professionalism, Warmth,
// Directness, Enthusiasm, Humor, Sarcasm, Pretentiousness); each shows the WORD
// for its value (Never..Always) via Personality.wordForValue(value), and binds
// to / writes back the matching kcm.personality* property. The page itself
// needs the C++ `kcm` context object and can't be instantiated headless (it's
// compile-probed in tst_QmlComponentsLoad), but the int<->word mapping and the
// trait table are pure logic, so we pin them directly — same approach as
// tst_VoiceBackends.
//
// The contract under test:
//   * the 5-step scale maps each value to its expected word, in order,
//   * out-of-range / non-integer / null input clamps to a valid step (the
//     label never renders blank/"undefined"; the daemon also rejects
//     out-of-range writes with InvalidArgs, so the UI stays inside 0..4),
//   * the trait table is in the issue's display order with the documented
//     built-in defaults (Professionalism=4, Warmth=3, Directness=3,
//     Enthusiasm=2, Humor=2, Sarcasm=1, Pretentiousness=1), and every trait's
//     `prop` matches the kcm.personality* property the page binds.
TestCase {
    id: testCase
    name: "PersonalityPage"

    // --- Word scale ---------------------------------------------------------

    function test_words_are_the_five_steps_in_order() {
        compare(Personality.WORDS,
                ["Never", "Rarely", "Sometimes", "Often", "Always"])
    }

    function test_word_for_value_maps_each_step() {
        compare(Personality.wordForValue(0), "Never")
        compare(Personality.wordForValue(1), "Rarely")
        compare(Personality.wordForValue(2), "Sometimes")
        compare(Personality.wordForValue(3), "Often")
        compare(Personality.wordForValue(4), "Always")
    }

    function test_word_for_value_clamps_out_of_range() {
        // Below 0 -> first step; above 4 -> last step. Never blank.
        compare(Personality.wordForValue(-1), "Never")
        compare(Personality.wordForValue(5), "Always")
        compare(Personality.wordForValue(99), "Always")
    }

    function test_word_for_value_rounds_and_is_null_safe() {
        // Slider drag can hand us a fractional value mid-snap; round it.
        compare(Personality.wordForValue(2.4), "Sometimes")
        compare(Personality.wordForValue(2.6), "Often")
        // Defensive: null/undefined/NaN must not throw or render "undefined".
        compare(Personality.wordForValue(null), "Never")
        compare(Personality.wordForValue(undefined), "Never")
        compare(Personality.wordForValue(NaN), "Never")
    }

    function test_clamp_step_keeps_valid_integer_range() {
        compare(Personality.clampStep(0), 0)
        compare(Personality.clampStep(4), 4)
        compare(Personality.clampStep(-3), 0)
        compare(Personality.clampStep(7), 4)
        compare(Personality.clampStep(3.5), 4)
    }

    // --- Trait table --------------------------------------------------------

    function test_traits_in_display_order() {
        var labels = Personality.TRAITS.map(function(t) { return t.label })
        compare(labels, [
            "Professionalism", "Warmth", "Directness", "Enthusiasm",
            "Humor", "Sarcasm", "Pretentiousness",
        ])
    }

    function test_trait_defaults_match_built_in_disposition() {
        var byLabel = {}
        for (var i = 0; i < Personality.TRAITS.length; i++) {
            byLabel[Personality.TRAITS[i].label] = Personality.TRAITS[i].default
        }
        compare(byLabel["Professionalism"], 4)
        compare(byLabel["Warmth"], 3)
        compare(byLabel["Directness"], 3)
        compare(byLabel["Enthusiasm"], 2)
        compare(byLabel["Humor"], 2)
        compare(byLabel["Sarcasm"], 1)
        compare(byLabel["Pretentiousness"], 1)
    }

    function test_trait_props_bind_kcm_personality_properties() {
        // Each trait's `prop` is the kcm Q_PROPERTY the slider binds to / writes
        // back; pin the exact names so the page and the C++ side can't drift.
        var props = Personality.TRAITS.map(function(t) { return t.prop })
        compare(props, [
            "personalityProfessionalism",
            "personalityWarmth",
            "personalityDirectness",
            "personalityEnthusiasm",
            "personalityHumor",
            "personalitySarcasm",
            "personalityPretentiousness",
        ])
    }

    function test_every_default_is_a_valid_step() {
        // Defaults must all be inside 0..4 so wordForValue never clamps them.
        for (var i = 0; i < Personality.TRAITS.length; i++) {
            var d = Personality.TRAITS[i].default
            verify(d >= 0 && d <= 4,
                   Personality.TRAITS[i].label + " default " + d + " out of 0..4")
            compare(Personality.clampStep(d), d)
        }
    }
}
