import QtQuick
import QtTest 1.0

import "../../shared/chat-module/ui" as Chat

// Load smoke test guarding adele-kde#18.
//
// #18 was a fatal QML load error in ChatView.qml — an unqualified attached
// `ToolTip` under an aliased `QtQuick.Controls as QQC2` import, reported by
// the engine as "Non-existent attached object". A fatal load error aborts
// rendering of the whole view, so the panel widget's popup came up blank
// (white). No test caught it: the suite covered Tasks* but not the much
// larger ChatView.
//
// This is a pure load/compile smoke test, not a behavior test. ChatView is
// self-contained — it drives a `Plasma5Support` executable DataSource rather
// than an injected backend — so no stub is needed. `Plasmoid.configuration`
// resolves to null outside a real plasmoid and only emits a runtime warning;
// it does not abort instantiation. `createTemporaryObject` returns null only
// when the component fails to compile/instantiate, which is exactly the
// failure mode we want to catch.
TestCase {
    id: testCase
    name: "ChatViewLoads"
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

    function test_chatview_instantiates_without_fatal_qml_error() {
        var view = createTemporaryObject(chatViewComponent, testCase)
        verify(view !== null, "ChatView instantiated without a fatal QML load error")
    }

    function test_chatview_panel_mode_also_loads() {
        // panelMode is the configuration the panel plasmoid uses (the one that
        // rendered white in #18).
        var view = createTemporaryObject(chatViewComponent, testCase, { panelMode: true })
        verify(view !== null, "ChatView (panelMode) instantiated without a fatal QML load error")
    }
}
