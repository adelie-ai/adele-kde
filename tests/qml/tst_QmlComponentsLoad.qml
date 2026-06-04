import QtQuick
import QtTest

// Load probe for the QML files that no other test exercises: the two
// plasmoid entry points (main.qml / PlasmoidItem), the five KCM files, and
// the two plasmoid config pages (configGeneral.qml). These are exactly the
// class of file the #18 white-popup bug hid in — a load-time QML error in a
// view that has no automated coverage.
//
// `plasmoidviewer` is not installed on the CI/dev box, so we drive the QML
// engine directly instead:
//
//   * Qt.createComponent(url) -> Component.Ready proves the file PARSES and
//     all of its imports/types/attached-objects RESOLVE. This catches the
//     #18-class "Non-existent attached object" error, missing imports,
//     unknown types and syntax errors — without needing a host context.
//   * For files with no hard host dependency (the configGeneral pages, whose
//     cfg_* are declared aliases), we additionally INSTANTIATE and assert the
//     object is non-null, which catches runtime load errors too.
//
// The plasmoid main.qml (needs the plasma shell) and the KCM pages (need the
// `kcm` context object) cannot be *instantiated* headless, so they are
// compile-probed only. That is the headless ceiling; full runtime-load
// testing of those would need plasmoidviewer / a systemsettings KCM harness.
TestCase {
    id: testCase
    name: "QmlComponentsLoad"

    function test_componentLoads_data() {
        return [
            // --- Plasmoid config pages: compile AND instantiate ---
            {
                tag: "desktopchat/configGeneral",
                path: "../../plasmoid/org.desktopassistant.desktopchat/contents/ui/configGeneral.qml",
                instantiate: true,
            },
            {
                tag: "panelchat/configGeneral",
                path: "../../plasmoid/org.desktopassistant.panelchat/contents/ui/configGeneral.qml",
                instantiate: true,
            },

            // --- Plasmoid entry points (PlasmoidItem): compile only ---
            {
                tag: "desktopchat/main",
                path: "../../plasmoid/org.desktopassistant.desktopchat/contents/ui/main.qml",
                instantiate: false,
            },
            {
                tag: "panelchat/main",
                path: "../../plasmoid/org.desktopassistant.panelchat/contents/ui/main.qml",
                instantiate: false,
            },

            // --- KCM pages (need the `kcm` context object): compile only ---
            {
                tag: "kcm/main",
                path: "../../kcm/desktop-assistant-settings/ui/main.qml",
                instantiate: false,
            },
            {
                tag: "kcm/ConnectionsPage",
                path: "../../kcm/desktop-assistant-settings/ui/ConnectionsPage.qml",
                instantiate: false,
            },
            {
                tag: "kcm/PurposesPage",
                path: "../../kcm/desktop-assistant-settings/ui/PurposesPage.qml",
                instantiate: false,
            },
            {
                tag: "kcm/KnowledgePage",
                path: "../../kcm/desktop-assistant-settings/ui/KnowledgePage.qml",
                instantiate: false,
            },
            {
                tag: "kcm/VoicePage",
                path: "../../kcm/desktop-assistant-settings/ui/VoicePage.qml",
                instantiate: false,
            },
            {
                tag: "kcm/ConnectionEditor",
                path: "../../kcm/desktop-assistant-settings/ui/ConnectionEditor.qml",
                instantiate: false,
            },
        ]
    }

    function test_componentLoads(data) {
        var url = Qt.resolvedUrl(data.path)
        var component = Qt.createComponent(url)

        // Local-file components resolve synchronously, but guard the async
        // path so a slow import never flakes the assertion below.
        if (component.status === Component.Loading) {
            tryVerify(function() { return component.status !== Component.Loading },
                      5000, data.tag + ": component stuck in Loading state")
        }

        verify(component.status === Component.Ready,
               data.tag + " failed to load: " + component.errorString())

        if (data.instantiate) {
            var obj = component.createObject(testCase)
            verify(obj !== null,
                   data.tag + " compiled but failed to instantiate: " + component.errorString())
            obj.destroy()
        }

        component.destroy()
    }
}
