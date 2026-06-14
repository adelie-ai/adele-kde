import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

// AbstractKCM, not SimpleKCM: SimpleKCM wraps the whole module in its own
// ScrollView, which double-scrolled against the per-tab scrolls (the tab bar
// and content scrolled as one ~2-screen-tall pane). AbstractKCM is a fixed
// viewport — the StackLayout fills it and each tab scrolls internally.
KCM.AbstractKCM {
    implicitWidth: 560
    implicitHeight: 460

    function indexForName(values, needle) {
        if (!values || values.length === 0) {
            return -1
        }
        for (let i = 0; i < values.length; i++) {
            if (values[i] === needle) {
                return i
            }
        }
        return -1
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        QQC2.Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
            font.italic: true
            opacity: 0.6
            text: "kcm_desktopassistant " + kcm.buildStamp
        }

        QQC2.TabBar {
            id: tabs
            Layout.fillWidth: true

            // The legacy single-LLM "Chat LLM" tab is gone (removed with
            // desktop-assistant#17; the daemon still exposes Get/SetLlmSettings
            // but this KCM no longer uses them). Connections + Purposes
            // replace it.
            QQC2.TabButton { text: "Connections" }
            QQC2.TabButton { text: "Purposes" }
            QQC2.TabButton { text: "Knowledge" }
            QQC2.TabButton { text: "Voice" }
            QQC2.TabButton { text: "Personality" }
            QQC2.TabButton { text: "Backend Tasks" }
            QQC2.TabButton { text: "Data Sync" }
            QQC2.TabButton { text: "Daemon Instances" }
            QQC2.TabButton { text: "Authentication" }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            QQC2.ScrollView {
                id: connectionsScroll
                clip: true
                contentWidth: availableWidth
                ConnectionsPage {
                    width: connectionsScroll.availableWidth
                    height: connectionsScroll.availableHeight
                }
            }

            QQC2.ScrollView {
                id: purposesScroll
                clip: true
                contentWidth: availableWidth
                PurposesPage {
                    width: purposesScroll.availableWidth
                    height: purposesScroll.availableHeight
                }
            }

            // No outer ScrollView here: the Knowledge page owns its own
            // scrolling — a resizable, scrollable list pane on the left and a
            // scrollable editor on the right. Wrapping it in a ScrollView gave
            // nested/competing scrollbars and squashed the inner panes.
            KnowledgePage {}

            QQC2.ScrollView {
                id: voiceScroll
                clip: true
                contentWidth: availableWidth
                VoicePage {
                    width: voiceScroll.availableWidth
                }
            }

            QQC2.ScrollView {
                id: personalityScroll
                clip: true
                contentWidth: availableWidth
                PersonalityPage {
                    width: personalityScroll.availableWidth
                }
            }

            QQC2.ScrollView {
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        QQC2.Label {
                            text: "Schedule & retention"
                            font.bold: true
                        }
                        InfoTip {
                            text: "Backend tasks (title generation, summary compaction, dreaming) use the connection and model assigned to their purpose on the Purposes tab. This page configures the schedule and retention policy."
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Kirigami.Separator { Layout.fillWidth: true }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        QQC2.Label {
                            text: "Dreaming"
                            font.bold: true
                        }
                        InfoTip {
                            text: "Dreaming periodically reviews conversations and extracts long-term facts into the knowledge base."
                        }
                        Item { Layout.fillWidth: true }
                    }

                    QQC2.CheckBox {
                        id: btDreamingEnabledCheck
                        text: "Enable dreaming"
                        checked: kcm.btDreamingEnabled
                        onToggled: kcm.btDreamingEnabled = checked
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: btDreamingEnabledCheck.checked
                        QQC2.Label { text: "Interval (seconds)" }
                        QQC2.SpinBox {
                            id: btDreamingIntervalBox
                            from: 60
                            to: 86400
                            stepSize: 300
                            value: kcm.btDreamingIntervalSecs
                            onValueModified: kcm.btDreamingIntervalSecs = value
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label { text: "Archive conversations after (days; 0 = never)" }
                        QQC2.SpinBox {
                            id: btArchiveAfterDaysBox
                            from: 0
                            to: 3650
                            stepSize: 1
                            value: kcm.btArchiveAfterDays
                            onValueModified: kcm.btArchiveAfterDays = value
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            QQC2.ScrollView {
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: 12

                    Kirigami.Separator { Layout.fillWidth: true }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        QQC2.Label {
                            text: "Database"
                            font.bold: true
                        }
                        InfoTip {
                            text: "Optional PostgreSQL database for structured storage. Leave the URL empty to use the built-in SQLite default."
                        }
                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label { text: "URL" }
                        QQC2.TextField {
                            id: dbUrlField
                            Layout.fillWidth: true
                            placeholderText: "postgres://user:pass@localhost/dbname"
                            text: kcm.dbUrl
                            onTextEdited: kcm.dbUrl = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label { text: "Max Connections" }
                        QQC2.SpinBox {
                            id: dbMaxConnectionsBox
                            from: 1
                            to: 100
                            value: kcm.dbMaxConnections
                            onValueModified: kcm.dbMaxConnections = value
                        }
                    }

                    Kirigami.Separator { Layout.fillWidth: true }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        QQC2.Label {
                            text: "Git Versioning"
                            font.bold: true
                        }
                        InfoTip {
                            text: "Version built-in memory and preferences in a local git repository. Optionally push each update to a remote for backup."
                        }
                        Item { Layout.fillWidth: true }
                    }

                    QQC2.CheckBox {
                        id: gitEnabledCheck
                        text: "Enable git versioning for data directory"
                        checked: kcm.gitEnabled
                        onToggled: kcm.gitEnabled = checked
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: gitEnabledCheck.checked
                        QQC2.Label { text: "Remote URL" }
                        QQC2.TextField {
                            id: gitRemoteUrlField
                            Layout.fillWidth: true
                            placeholderText: "git@github.com:you/assistant-memory.git (optional)"
                            text: kcm.gitRemoteUrl
                            onTextEdited: kcm.gitRemoteUrl = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: gitEnabledCheck.checked
                        QQC2.Label { text: "Remote name" }
                        QQC2.TextField {
                            id: gitRemoteNameField
                            Layout.fillWidth: true
                            placeholderText: "origin"
                            text: kcm.gitRemoteName
                            onTextEdited: kcm.gitRemoteName = text
                        }
                    }

                    QQC2.CheckBox {
                        id: gitPushOnUpdateCheck
                        enabled: gitEnabledCheck.checked && gitRemoteUrlField.text.trim() !== ""
                        text: "Push to remote on every update"
                        checked: kcm.gitPushOnUpdate
                        onToggled: kcm.gitPushOnUpdate = checked
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            QQC2.ScrollView {
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        QQC2.Label {
                            text: "Daemon instances"
                            font.bold: true
                        }
                        InfoTip {
                            text: "Define Adelie connections once, set a global default, and let each widget pick a connection by name. 'local' is the default fallback, but any configured connection can be selected as default."
                        }
                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label {
                            text: "Default"
                            Layout.preferredWidth: 120
                        }
                        QQC2.ComboBox {
                            id: defaultConnectionBox
                            Layout.fillWidth: true
                            model: kcm.connectionNames
                            currentIndex: Math.max(0, indexForName(kcm.connectionNames, kcm.defaultConnectionName))
                            onActivated: {
                                if (currentIndex >= 0) {
                                    kcm.defaultConnectionName = currentText
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label {
                            text: "Edit"
                            Layout.preferredWidth: 120
                        }
                        QQC2.ComboBox {
                            id: selectedConnectionBox
                            Layout.fillWidth: true
                            model: kcm.connectionNames
                            currentIndex: Math.max(0, indexForName(kcm.connectionNames, kcm.selectedConnectionName))
                            onActivated: {
                                if (currentIndex >= 0) {
                                    kcm.selectedConnectionName = currentText
                                }
                            }
                        }
                        QQC2.Button {
                            text: "Remove"
                            enabled: kcm.selectedConnectionRemovable
                            onClicked: kcm.removeSelectedConnection()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label {
                            text: "Add remote"
                            Layout.preferredWidth: 120
                        }
                        QQC2.TextField {
                            id: newConnectionNameField
                            Layout.fillWidth: true
                            placeholderText: "my-cluster"
                            onAccepted: addConnectionButton.clicked()
                        }
                        QQC2.Button {
                            id: addConnectionButton
                            text: "Add"
                            onClicked: {
                                const value = newConnectionNameField.text.trim()
                                if (value.length === 0) {
                                    return
                                }
                                kcm.addRemoteConnection(value)
                                newConnectionNameField.text = ""
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label {
                            text: "Transport"
                            Layout.preferredWidth: 120
                        }
                        QQC2.TextField {
                            Layout.fillWidth: true
                            readOnly: true
                            text: kcm.selectedConnectionTransport
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: kcm.selectedConnectionTransport === "dbus"
                        QQC2.Label {
                            text: "D-Bus service"
                            Layout.preferredWidth: 120
                        }
                        QQC2.TextField {
                            id: selectedConnectionDbusServiceField
                            Layout.fillWidth: true
                            placeholderText: "org.desktopAssistant"
                            text: kcm.selectedConnectionDbusService
                            onTextEdited: kcm.selectedConnectionDbusService = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: kcm.selectedConnectionTransport === "ws"
                        QQC2.Label {
                            text: "WebSocket URL"
                            Layout.preferredWidth: 120
                        }
                        QQC2.TextField {
                            id: selectedConnectionWsUrlField
                            Layout.fillWidth: true
                            placeholderText: "wss://cluster.example.com/ws"
                            text: kcm.selectedConnectionWsUrl
                            onTextEdited: kcm.selectedConnectionWsUrl = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: kcm.selectedConnectionTransport === "ws"
                        QQC2.Label {
                            text: "JWT subject"
                            Layout.preferredWidth: 120
                        }
                        QQC2.TextField {
                            id: selectedConnectionWsSubjectField
                            Layout.fillWidth: true
                            placeholderText: "desktop-widget"
                            text: kcm.selectedConnectionWsSubject
                            onTextEdited: kcm.selectedConnectionWsSubject = text
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            QQC2.ScrollView {
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        QQC2.Label {
                            text: "WebSocket authentication"
                            font.bold: true
                        }
                        InfoTip {
                            text: "Configure which authentication methods the WebSocket API accepts. Password uses local OS credentials or a static password. OIDC delegates to an external identity provider."
                        }
                        Item { Layout.fillWidth: true }
                    }

                    QQC2.Label {
                        font.bold: true
                        text: "Enabled Methods"
                    }

                    QQC2.CheckBox {
                        id: authPasswordCheck
                        text: "Password authentication"
                        checked: kcm.wsAuthPasswordEnabled
                        onToggled: kcm.wsAuthPasswordEnabled = checked
                    }

                    QQC2.CheckBox {
                        id: authOidcCheck
                        text: "OIDC / OAuth2 authentication"
                        checked: kcm.wsAuthOidcEnabled
                        onToggled: kcm.wsAuthOidcEnabled = checked
                    }

                    Kirigami.Separator { Layout.fillWidth: true }

                    QQC2.Label {
                        font.bold: true
                        text: "OIDC Configuration"
                        enabled: authOidcCheck.checked
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: authOidcCheck.checked
                        QQC2.Label {
                            text: "Issuer URL"
                            Layout.preferredWidth: 140
                        }
                        QQC2.TextField {
                            id: oidcIssuerField
                            Layout.fillWidth: true
                            placeholderText: "https://myapp.auth0.com"
                            text: kcm.oidcIssuer
                            onTextEdited: kcm.oidcIssuer = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: authOidcCheck.checked
                        QQC2.Label {
                            text: "Authorization Endpoint"
                            Layout.preferredWidth: 140
                        }
                        QQC2.TextField {
                            id: oidcAuthEndpointField
                            Layout.fillWidth: true
                            placeholderText: "https://myapp.auth0.com/authorize"
                            text: kcm.oidcAuthEndpoint
                            onTextEdited: kcm.oidcAuthEndpoint = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: authOidcCheck.checked
                        QQC2.Label {
                            text: "Token Endpoint"
                            Layout.preferredWidth: 140
                        }
                        QQC2.TextField {
                            id: oidcTokenEndpointField
                            Layout.fillWidth: true
                            placeholderText: "https://myapp.auth0.com/oauth/token"
                            text: kcm.oidcTokenEndpoint
                            onTextEdited: kcm.oidcTokenEndpoint = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: authOidcCheck.checked
                        QQC2.Label {
                            text: "Client ID"
                            Layout.preferredWidth: 140
                        }
                        QQC2.TextField {
                            id: oidcClientIdField
                            Layout.fillWidth: true
                            placeholderText: "abc123public"
                            text: kcm.oidcClientId
                            onTextEdited: kcm.oidcClientId = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        enabled: authOidcCheck.checked
                        QQC2.Label {
                            text: "Scopes"
                            Layout.preferredWidth: 140
                        }
                        QQC2.TextField {
                            id: oidcScopesField
                            Layout.fillWidth: true
                            placeholderText: "openid profile email"
                            text: kcm.oidcScopes
                            onTextEdited: kcm.oidcScopes = text
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: kcm.statusText
        }

        Connections {
            target: kcm

            // Chat-LLM text fields were removed alongside the legacy tab
            // (desktop-assistant#17); their model/baseUrl/apiKeyInput
            // sync handlers moved into the Connections + Purposes pages.

            function onDbUrlChanged() {
                if (dbUrlField.text !== kcm.dbUrl) {
                    dbUrlField.text = kcm.dbUrl
                }
            }

            function onDbMaxConnectionsChanged() {
                if (dbMaxConnectionsBox.value !== kcm.dbMaxConnections) {
                    dbMaxConnectionsBox.value = kcm.dbMaxConnections
                }
            }

            function onGitRemoteUrlChanged() {
                if (gitRemoteUrlField.text !== kcm.gitRemoteUrl) {
                    gitRemoteUrlField.text = kcm.gitRemoteUrl
                }
            }

            function onGitRemoteNameChanged() {
                if (gitRemoteNameField.text !== kcm.gitRemoteName) {
                    gitRemoteNameField.text = kcm.gitRemoteName
                }
            }

            function onSelectedConnectionDbusServiceChanged() {
                if (selectedConnectionDbusServiceField.text !== kcm.selectedConnectionDbusService) {
                    selectedConnectionDbusServiceField.text = kcm.selectedConnectionDbusService
                }
            }

            function onSelectedConnectionWsUrlChanged() {
                if (selectedConnectionWsUrlField.text !== kcm.selectedConnectionWsUrl) {
                    selectedConnectionWsUrlField.text = kcm.selectedConnectionWsUrl
                }
            }

            function onSelectedConnectionWsSubjectChanged() {
                if (selectedConnectionWsSubjectField.text !== kcm.selectedConnectionWsSubject) {
                    selectedConnectionWsSubjectField.text = kcm.selectedConnectionWsSubject
                }
            }

            function onBtDreamingIntervalSecsChanged() {
                if (btDreamingIntervalBox.value !== kcm.btDreamingIntervalSecs) {
                    btDreamingIntervalBox.value = kcm.btDreamingIntervalSecs
                }
            }

            function onBtArchiveAfterDaysChanged() {
                if (btArchiveAfterDaysBox.value !== kcm.btArchiveAfterDays) {
                    btArchiveAfterDaysBox.value = kcm.btArchiveAfterDays
                }
            }

            function onOidcIssuerChanged() {
                if (oidcIssuerField.text !== kcm.oidcIssuer) {
                    oidcIssuerField.text = kcm.oidcIssuer
                }
            }

            function onOidcAuthEndpointChanged() {
                if (oidcAuthEndpointField.text !== kcm.oidcAuthEndpoint) {
                    oidcAuthEndpointField.text = kcm.oidcAuthEndpoint
                }
            }

            function onOidcTokenEndpointChanged() {
                if (oidcTokenEndpointField.text !== kcm.oidcTokenEndpoint) {
                    oidcTokenEndpointField.text = kcm.oidcTokenEndpoint
                }
            }

            function onOidcClientIdChanged() {
                if (oidcClientIdField.text !== kcm.oidcClientId) {
                    oidcClientIdField.text = kcm.oidcClientId
                }
            }

            function onOidcScopesChanged() {
                if (oidcScopesField.text !== kcm.oidcScopes) {
                    oidcScopesField.text = kcm.oidcScopes
                }
            }
        }
    }
}
