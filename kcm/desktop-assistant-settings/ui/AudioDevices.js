.pragma library

// Pure mapping helper for the Voice page's audio-device selectors
// (adele-kde#37). Factored out of VoicePage.qml so the value<->row mapping can
// be unit-tested without instantiating the page (which needs the C++ `kcm`
// context object and so is only compile-probed headless).
//
// The KCM provides each device-option list as an array of {value, label} maps
// whose first entry is always "Follow system default" (value "default"). The
// page binds the ComboBox `currentIndex` to deviceIndexByValue(options, value)
// and, on selection, writes back `options[currentIndex].value`.

// Row of `value` within an {value,label}[] option list. Unknown/empty/missing
// values (and a null/undefined list) fall back to row 0 — which is the leading
// "Follow system default" entry — so the ComboBox never renders blank or
// selects nothing.
function deviceIndexByValue(options, value) {
    var list = options || []
    for (var i = 0; i < list.length; i++) {
        if (String(list[i].value) === String(value)) {
            return i
        }
    }
    return 0
}
