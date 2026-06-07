import QtQuick
import QtTest 1.0

import "../../kcm/desktop-assistant-settings/ui/AudioDevices.js" as AudioDevices

// Unit tests for the Voice page's audio-device selector mapping (adele-kde#37).
//
// VoicePage.qml binds the input/output ComboBoxes' `currentIndex` to
// AudioDevices.deviceIndexByValue(kcm.<dir>DeviceOptions, kcm.<dir>Device) and
// on selection writes back `options[currentIndex].value`. The page itself needs
// the C++ `kcm` context object and can't be instantiated headless (it's
// compile-probed in tst_QmlComponentsLoad), but this value<->row mapping is pure
// logic, so we pin it directly — the same approach as tst_VoiceBackends.
//
// The contract under test:
//   * a stored value maps to its row in the {value,label}[] option list,
//   * the leading "Follow system default" entry (value "default") is row 0,
//   * unknown/empty values (and a null/undefined list) fall back to row 0 so
//     the ComboBox never renders blank or selects nothing.
TestCase {
    id: testCase
    name: "AudioDevices"

    // Mirrors the KCM-built option list: "default" first, then real devices.
    readonly property var options: [
        { value: "default", label: "Follow system default (recommended)" },
        { value: "Mini", label: "Razer Seiren V3 Mini Mono" },
        { value: "PCH", label: "Built-in Audio Analog Stereo" },
    ]

    function test_default_is_row_zero() {
        compare(AudioDevices.deviceIndexByValue(options, "default"), 0)
    }

    function test_known_device_maps_to_its_row() {
        compare(AudioDevices.deviceIndexByValue(options, "Mini"), 1)
        compare(AudioDevices.deviceIndexByValue(options, "PCH"), 2)
    }

    function test_unknown_value_falls_back_to_default_row() {
        // A configured device that no longer enumerates must not select -1
        // (blank combo); fall back to the default row.
        compare(AudioDevices.deviceIndexByValue(options, "Bluetooth"), 0)
    }

    function test_empty_value_falls_back_to_default_row() {
        compare(AudioDevices.deviceIndexByValue(options, ""), 0)
    }

    function test_null_and_undefined_are_safe() {
        // Defensive: a null/undefined value must return 0, not throw.
        compare(AudioDevices.deviceIndexByValue(options, null), 0)
        compare(AudioDevices.deviceIndexByValue(options, undefined), 0)
    }

    function test_null_option_list_is_safe() {
        // Before loadAudioDevices() populates the list it may be null/empty;
        // the mapping must still return 0 rather than throw.
        compare(AudioDevices.deviceIndexByValue(null, "Mini"), 0)
        compare(AudioDevices.deviceIndexByValue(undefined, "Mini"), 0)
        compare(AudioDevices.deviceIndexByValue([], "Mini"), 0)
    }

    function test_value_compared_as_string() {
        // Values arrive from C++ QVariant maps; compare stringwise so a numeric
        // token and its string form still match (and never throw).
        var numeric = [{ value: "default", label: "Default" }, { value: 7, label: "Seven" }]
        compare(AudioDevices.deviceIndexByValue(numeric, "7"), 1)
        compare(AudioDevices.deviceIndexByValue(numeric, 7), 1)
    }
}
