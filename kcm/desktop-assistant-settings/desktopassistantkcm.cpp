#include "desktopassistantkcm.h"

#include <algorithm>
#include <dlfcn.h>
#include <sys/stat.h>
#include <QDateTime>

#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <KPluginFactory>

namespace {
constexpr auto SERVICE = "org.desktopAssistant";
constexpr auto PATH = "/org/desktopAssistant/Settings";
constexpr auto IFACE = "org.desktopAssistant.Settings";
constexpr auto DEFAULT_CONNECTION_NAME = "local";
constexpr auto DEFAULT_WS_URL = "ws://127.0.0.1:11339/ws";
constexpr auto DEFAULT_WS_SUBJECT = "desktop-widget";
// Voice daemon (repo adelie-ai/voice). Distinct bus name from the orchestrator.
constexpr auto VOICE_SERVICE = "org.desktopAssistant.Voice";
constexpr auto VOICE_PATH = "/org/desktopAssistant/Voice";
constexpr auto VOICE_IFACE = "org.desktopAssistant.Voice";
constexpr auto VOICE_UNIT = "adele-voice.service";

QString normalizeConnector(const QString &connector)
{
    const auto normalized = connector.trimmed().toLower();
    return normalized.isEmpty() ? QStringLiteral("openai") : normalized;
}

QString widgetSettingsPath()
{
    const auto configHome = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return QDir(configHome).filePath(QStringLiteral("desktop-assistant/widget_settings.json"));
}

QString normalizeConnectionName(const QString &name)
{
    return name.trimmed();
}

struct ConnectorDefaults {
    QString llmModel;
    QString llmBaseUrl;
    QString embeddingsModel;
    QString embeddingsBaseUrl;
    bool embeddingsAvailable = true;
    bool hostedToolSearchAvailable = true;
    QString backendLlmModel;
};

bool fetchConnectorDefaults(
    QDBusInterface &iface,
    const QString &connector,
    ConnectorDefaults *out,
    QString *errorText
)
{
    QDBusMessage reply = iface.call("GetConnectorDefaults", connector);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (errorText != nullptr) {
            *errorText = reply.errorMessage().isEmpty() ? QStringLiteral("D-Bus call failed") : reply.errorMessage();
        }
        return false;
    }

    const auto args = reply.arguments();
    if (args.size() < 5) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Unexpected GetConnectorDefaults reply");
        }
        return false;
    }

    if (out != nullptr) {
        out->llmModel = args[0].toString();
        out->llmBaseUrl = args[1].toString();
        out->embeddingsModel = args[2].toString();
        out->embeddingsBaseUrl = args[3].toString();
        out->embeddingsAvailable = args[4].toBool();
        out->hostedToolSearchAvailable = args.size() > 5 ? args[5].toBool() : true;
        out->backendLlmModel = args.size() > 6 ? args[6].toString() : out->llmModel;
    }

    return true;
}
}

K_PLUGIN_CLASS_WITH_JSON(DesktopAssistantKcm, "kcm_desktopassistant.json")

DesktopAssistantKcm::DesktopAssistantKcm(QObject *parent, const KPluginMetaData &metaData, const QVariantList &args)
    : KQuickConfigModule(parent, metaData)
{
    Q_UNUSED(args);
    // Immediate-save throughout: each setter fires its own D-Bus call, so
    // the System Settings "Apply" chrome would be misleading. The daemon
    // already hot-reloads each Set* method, so no separate Apply gesture
    // (or daemon restart) is needed.
    setButtons(NoAdditionalButton);
    load();
}

QString DesktopAssistantKcm::buildStamp() const
{
    // Reads the .so's own mtime so every reinstall produces a fresh stamp
    // visible from QML — even when only QML files changed in this build.
    // (__DATE__/__TIME__ would only refresh when this translation unit
    // itself recompiled, which doesn't happen for QML-only edits.)
    Dl_info info;
    if (dladdr(reinterpret_cast<const void *>(&DesktopAssistantKcm::staticMetaObject), &info) && info.dli_fname) {
        struct stat st {};
        if (stat(info.dli_fname, &st) == 0) {
            const auto dt = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(st.st_mtime));
            return QStringLiteral("built ") + dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        }
    }
    return QStringLiteral("built (unknown)");
}

QString DesktopAssistantKcm::connector() const
{
    return m_connector;
}

void DesktopAssistantKcm::setConnector(const QString &value)
{
    if (m_connector == value) {
        return;
    }

    m_connector = value;
    Q_EMIT connectorChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

QString DesktopAssistantKcm::model() const
{
    return m_model;
}

void DesktopAssistantKcm::setModel(const QString &value)
{
    if (m_model == value) {
        return;
    }

    m_model = value;
    Q_EMIT modelChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

QString DesktopAssistantKcm::baseUrl() const
{
    return m_baseUrl;
}

void DesktopAssistantKcm::setBaseUrl(const QString &value)
{
    if (m_baseUrl == value) {
        return;
    }

    m_baseUrl = value;
    Q_EMIT baseUrlChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

QString DesktopAssistantKcm::embConnector() const
{
    return m_embConnector;
}

void DesktopAssistantKcm::setEmbConnector(const QString &value)
{
    if (m_embConnector == value) {
        return;
    }

    m_embConnector = value;
    Q_EMIT embConnectorChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

QString DesktopAssistantKcm::embModel() const
{
    return m_embModel;
}

void DesktopAssistantKcm::setEmbModel(const QString &value)
{
    if (m_embModel == value) {
        return;
    }

    m_embModel = value;
    Q_EMIT embModelChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

QString DesktopAssistantKcm::embBaseUrl() const
{
    return m_embBaseUrl;
}

void DesktopAssistantKcm::setEmbBaseUrl(const QString &value)
{
    if (m_embBaseUrl == value) {
        return;
    }

    m_embBaseUrl = value;
    Q_EMIT embBaseUrlChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

bool DesktopAssistantKcm::embHasApiKey() const
{
    return m_embHasApiKey;
}

bool DesktopAssistantKcm::embAvailable() const
{
    return m_embAvailable;
}

bool DesktopAssistantKcm::embIsDefault() const
{
    return m_embIsDefault;
}

QString DesktopAssistantKcm::apiKeyInput() const
{
    return m_apiKeyInput;
}

void DesktopAssistantKcm::setApiKeyInput(const QString &value)
{
    if (m_apiKeyInput == value) {
        return;
    }

    m_apiKeyInput = value;
    Q_EMIT apiKeyInputChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

bool DesktopAssistantKcm::hasApiKey() const
{
    return m_hasApiKey;
}

QString DesktopAssistantKcm::statusText() const
{
    return m_statusText;
}

bool DesktopAssistantKcm::gitEnabled() const
{
    return m_gitEnabled;
}

void DesktopAssistantKcm::setGitEnabled(bool value)
{
    if (m_gitEnabled == value) {
        return;
    }
    m_gitEnabled = value;
    Q_EMIT gitEnabledChanged();
    pushPersistenceSettings();
}

QString DesktopAssistantKcm::gitRemoteUrl() const
{
    return m_gitRemoteUrl;
}

void DesktopAssistantKcm::setGitRemoteUrl(const QString &value)
{
    if (m_gitRemoteUrl == value) {
        return;
    }
    m_gitRemoteUrl = value;
    Q_EMIT gitRemoteUrlChanged();
    pushPersistenceSettings();
}

QString DesktopAssistantKcm::gitRemoteName() const
{
    return m_gitRemoteName;
}

void DesktopAssistantKcm::setGitRemoteName(const QString &value)
{
    if (m_gitRemoteName == value) {
        return;
    }
    m_gitRemoteName = value;
    Q_EMIT gitRemoteNameChanged();
    pushPersistenceSettings();
}

bool DesktopAssistantKcm::gitPushOnUpdate() const
{
    return m_gitPushOnUpdate;
}

void DesktopAssistantKcm::setGitPushOnUpdate(bool value)
{
    if (m_gitPushOnUpdate == value) {
        return;
    }
    m_gitPushOnUpdate = value;
    Q_EMIT gitPushOnUpdateChanged();
    pushPersistenceSettings();
}

QString DesktopAssistantKcm::dbUrl() const
{
    return m_dbUrl;
}

void DesktopAssistantKcm::setDbUrl(const QString &value)
{
    if (m_dbUrl == value) {
        return;
    }
    m_dbUrl = value;
    Q_EMIT dbUrlChanged();
    pushDatabaseSettings();
}

int DesktopAssistantKcm::dbMaxConnections() const
{
    return m_dbMaxConnections;
}

void DesktopAssistantKcm::setDbMaxConnections(int value)
{
    if (m_dbMaxConnections == value) {
        return;
    }
    m_dbMaxConnections = value;
    Q_EMIT dbMaxConnectionsChanged();
    pushDatabaseSettings();
}

QStringList DesktopAssistantKcm::connectionNames() const
{
    QStringList names;
    names.reserve(m_connections.size());
    for (const auto &connection : m_connections) {
        names.push_back(connection.name);
    }
    return names;
}

QString DesktopAssistantKcm::defaultConnectionName() const
{
    return m_defaultConnectionName;
}

void DesktopAssistantKcm::setDefaultConnectionName(const QString &value)
{
    const auto normalized = normalizeConnectionName(value);
    if (normalized.isEmpty() || connectionIndexByName(normalized) < 0) {
        return;
    }
    if (m_defaultConnectionName == normalized) {
        return;
    }

    m_defaultConnectionName = normalized;
    Q_EMIT defaultConnectionNameChanged();
    saveWidgetConnectionSettings();
}

QString DesktopAssistantKcm::selectedConnectionName() const
{
    return m_selectedConnectionName;
}

void DesktopAssistantKcm::setSelectedConnectionName(const QString &value)
{
    const auto normalized = normalizeConnectionName(value);
    const auto index = connectionIndexByName(normalized);
    if (index < 0) {
        return;
    }
    setSelectedConnectionByIndex(index);
}

QString DesktopAssistantKcm::selectedConnectionTransport() const
{
    const auto index = selectedConnectionIndex();
    if (index < 0) {
        return QStringLiteral("dbus");
    }
    return m_connections[index].transport;
}

QString DesktopAssistantKcm::selectedConnectionDbusService() const
{
    const auto index = selectedConnectionIndex();
    if (index < 0) {
        return QString::fromUtf8(SERVICE);
    }
    return m_connections[index].dbusService;
}

void DesktopAssistantKcm::setSelectedConnectionDbusService(const QString &value)
{
    const auto index = selectedConnectionIndex();
    if (index < 0 || m_connections[index].transport != QLatin1String("dbus")) {
        return;
    }

    const auto normalized = value.trimmed().isEmpty() ? QString::fromUtf8(SERVICE) : value.trimmed();
    if (m_connections[index].dbusService == normalized) {
        return;
    }

    m_connections[index].dbusService = normalized;
    Q_EMIT selectedConnectionDbusServiceChanged();
    saveWidgetConnectionSettings();
}

QString DesktopAssistantKcm::selectedConnectionWsUrl() const
{
    const auto index = selectedConnectionIndex();
    if (index < 0) {
        return QString::fromUtf8(DEFAULT_WS_URL);
    }
    return m_connections[index].wsUrl;
}

void DesktopAssistantKcm::setSelectedConnectionWsUrl(const QString &value)
{
    const auto index = selectedConnectionIndex();
    if (index < 0 || m_connections[index].transport != QLatin1String("ws")) {
        return;
    }

    const auto normalized = value.trimmed().isEmpty() ? QString::fromUtf8(DEFAULT_WS_URL) : value.trimmed();
    if (m_connections[index].wsUrl == normalized) {
        return;
    }

    m_connections[index].wsUrl = normalized;
    Q_EMIT selectedConnectionWsUrlChanged();
    saveWidgetConnectionSettings();
}

QString DesktopAssistantKcm::selectedConnectionWsSubject() const
{
    const auto index = selectedConnectionIndex();
    if (index < 0) {
        return QString::fromUtf8(DEFAULT_WS_SUBJECT);
    }
    return m_connections[index].wsSubject;
}

void DesktopAssistantKcm::setSelectedConnectionWsSubject(const QString &value)
{
    const auto index = selectedConnectionIndex();
    if (index < 0 || m_connections[index].transport != QLatin1String("ws")) {
        return;
    }

    const auto normalized = value.trimmed().isEmpty() ? QString::fromUtf8(DEFAULT_WS_SUBJECT) : value.trimmed();
    if (m_connections[index].wsSubject == normalized) {
        return;
    }

    m_connections[index].wsSubject = normalized;
    Q_EMIT selectedConnectionWsSubjectChanged();
    saveWidgetConnectionSettings();
}

bool DesktopAssistantKcm::selectedConnectionRemovable() const
{
    return m_connections.size() > 1;
}

bool DesktopAssistantKcm::btDreamingEnabled() const
{
    return m_btDreamingEnabled;
}

void DesktopAssistantKcm::setBtDreamingEnabled(bool value)
{
    if (m_btDreamingEnabled == value) {
        return;
    }
    m_btDreamingEnabled = value;
    Q_EMIT btDreamingEnabledChanged();
    pushBackendTasksSettings();
}

int DesktopAssistantKcm::btDreamingIntervalSecs() const
{
    return m_btDreamingIntervalSecs;
}

void DesktopAssistantKcm::setBtDreamingIntervalSecs(int value)
{
    if (m_btDreamingIntervalSecs == value) {
        return;
    }
    m_btDreamingIntervalSecs = value;
    Q_EMIT btDreamingIntervalSecsChanged();
    pushBackendTasksSettings();
}

int DesktopAssistantKcm::btArchiveAfterDays() const
{
    return m_btArchiveAfterDays;
}

void DesktopAssistantKcm::setBtArchiveAfterDays(int value)
{
    if (m_btArchiveAfterDays == value) {
        return;
    }
    m_btArchiveAfterDays = value;
    Q_EMIT btArchiveAfterDaysChanged();
    pushBackendTasksSettings();
}

bool DesktopAssistantKcm::btHasSeparateLlm() const
{
    return m_btHasSeparateLlm;
}

QString DesktopAssistantKcm::btLlmConnector() const
{
    return m_btLlmConnector;
}

void DesktopAssistantKcm::setBtLlmConnector(const QString &value)
{
    if (m_btLlmConnector == value) {
        return;
    }
    m_btLlmConnector = value;
    Q_EMIT btLlmConnectorChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

QString DesktopAssistantKcm::btLlmModel() const
{
    return m_btLlmModel;
}

void DesktopAssistantKcm::setBtLlmModel(const QString &value)
{
    if (m_btLlmModel == value) {
        return;
    }
    m_btLlmModel = value;
    Q_EMIT btLlmModelChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

QString DesktopAssistantKcm::btLlmBaseUrl() const
{
    return m_btLlmBaseUrl;
}

void DesktopAssistantKcm::setBtLlmBaseUrl(const QString &value)
{
    if (m_btLlmBaseUrl == value) {
        return;
    }
    m_btLlmBaseUrl = value;
    Q_EMIT btLlmBaseUrlChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

int DesktopAssistantKcm::hostedToolSearch() const
{
    return m_hostedToolSearch;
}

void DesktopAssistantKcm::setHostedToolSearch(int value)
{
    if (m_hostedToolSearch == value) {
        return;
    }
    m_hostedToolSearch = value;
    Q_EMIT hostedToolSearchChanged();
    // Vestigial setter; UI no longer binds this property. No-op save path.
}

bool DesktopAssistantKcm::hostedToolSearchAvailable() const
{
    return m_hostedToolSearchAvailable;
}

bool DesktopAssistantKcm::wsAuthPasswordEnabled() const
{
    return m_wsAuthMethods.contains(QStringLiteral("password"));
}

void DesktopAssistantKcm::setWsAuthPasswordEnabled(bool value)
{
    const auto method = QStringLiteral("password");
    if (value && !m_wsAuthMethods.contains(method)) {
        m_wsAuthMethods.append(method);
        Q_EMIT wsAuthMethodsChanged();
        pushWsAuthSettings();
    } else if (!value && m_wsAuthMethods.contains(method)) {
        m_wsAuthMethods.removeAll(method);
        Q_EMIT wsAuthMethodsChanged();
        pushWsAuthSettings();
    }
}

bool DesktopAssistantKcm::wsAuthOidcEnabled() const
{
    return m_wsAuthMethods.contains(QStringLiteral("oidc"));
}

void DesktopAssistantKcm::setWsAuthOidcEnabled(bool value)
{
    const auto method = QStringLiteral("oidc");
    if (value && !m_wsAuthMethods.contains(method)) {
        m_wsAuthMethods.append(method);
        Q_EMIT wsAuthMethodsChanged();
        pushWsAuthSettings();
    } else if (!value && m_wsAuthMethods.contains(method)) {
        m_wsAuthMethods.removeAll(method);
        Q_EMIT wsAuthMethodsChanged();
        pushWsAuthSettings();
    }
}

QString DesktopAssistantKcm::oidcIssuer() const { return m_oidcIssuer; }
void DesktopAssistantKcm::setOidcIssuer(const QString &value)
{
    if (m_oidcIssuer == value) return;
    m_oidcIssuer = value;
    Q_EMIT oidcIssuerChanged();
    pushWsAuthSettings();
}

QString DesktopAssistantKcm::oidcAuthEndpoint() const { return m_oidcAuthEndpoint; }
void DesktopAssistantKcm::setOidcAuthEndpoint(const QString &value)
{
    if (m_oidcAuthEndpoint == value) return;
    m_oidcAuthEndpoint = value;
    Q_EMIT oidcAuthEndpointChanged();
    pushWsAuthSettings();
}

QString DesktopAssistantKcm::oidcTokenEndpoint() const { return m_oidcTokenEndpoint; }
void DesktopAssistantKcm::setOidcTokenEndpoint(const QString &value)
{
    if (m_oidcTokenEndpoint == value) return;
    m_oidcTokenEndpoint = value;
    Q_EMIT oidcTokenEndpointChanged();
    pushWsAuthSettings();
}

QString DesktopAssistantKcm::oidcClientId() const { return m_oidcClientId; }
void DesktopAssistantKcm::setOidcClientId(const QString &value)
{
    if (m_oidcClientId == value) return;
    m_oidcClientId = value;
    Q_EMIT oidcClientIdChanged();
    pushWsAuthSettings();
}

QString DesktopAssistantKcm::oidcScopes() const { return m_oidcScopes; }
void DesktopAssistantKcm::setOidcScopes(const QString &value)
{
    if (m_oidcScopes == value) return;
    m_oidcScopes = value;
    Q_EMIT oidcScopesChanged();
    pushWsAuthSettings();
}

void DesktopAssistantKcm::load()
{
    QDBusInterface iface(SERVICE, PATH, IFACE, QDBusConnection::sessionBus());
    QDBusMessage reply = iface.call("GetLlmSettings");

    if (setStatusFromDbusError(reply)) {
        return;
    }

    const auto args = reply.arguments();
    if (args.size() < 4) {
        m_statusText = QStringLiteral("Unexpected GetLlmSettings reply");
        Q_EMIT statusTextChanged();
        return;
    }

    m_connector = args[0].toString();
    m_model = args[1].toString();
    m_baseUrl = args[2].toString();
    m_hasApiKey = args[3].toBool();
    // hosted_tool_search: -1 = connector default, 0 = off, 1 = on (8th arg)
    m_hostedToolSearch = args.size() > 7 ? args[7].toInt() : -1;

    QDBusMessage embReply = iface.call("GetEmbeddingsSettings");
    if (setStatusFromDbusError(embReply)) {
        return;
    }

    const auto embArgs = embReply.arguments();
    if (embArgs.size() < 6) {
        m_statusText = QStringLiteral("Unexpected GetEmbeddingsSettings reply");
        Q_EMIT statusTextChanged();
        return;
    }

    m_embConnector = embArgs[5].toBool() ? QString() : embArgs[0].toString();
    m_embModel = embArgs[1].toString();
    m_embBaseUrl = embArgs[2].toString();
    m_embHasApiKey = embArgs[3].toBool();
    m_embAvailable = embArgs[4].toBool();
    m_embIsDefault = embArgs[5].toBool();

    m_apiKeyInput.clear();

    QDBusMessage gitReply = iface.call("GetPersistenceSettings");
    if (setStatusFromDbusError(gitReply)) {
        return;
    }

    const auto gitArgs = gitReply.arguments();
    if (gitArgs.size() < 4) {
        m_statusText = QStringLiteral("Unexpected GetPersistenceSettings reply");
        Q_EMIT statusTextChanged();
        return;
    }

    m_gitEnabled = gitArgs[0].toBool();
    m_gitRemoteUrl = gitArgs[1].toString();
    m_gitRemoteName = gitArgs[2].toString();
    m_gitPushOnUpdate = gitArgs[3].toBool();

    QDBusMessage dbReply = iface.call("GetDatabaseSettings");
    if (setStatusFromDbusError(dbReply)) {
        return;
    }

    const auto dbArgs = dbReply.arguments();
    if (dbArgs.size() < 2) {
        m_statusText = QStringLiteral("Unexpected GetDatabaseSettings reply");
        Q_EMIT statusTextChanged();
        return;
    }

    m_dbUrl = dbArgs[0].toString();
    m_dbMaxConnections = dbArgs[1].toInt();

    QDBusMessage btReply = iface.call("GetBackendTasksSettings");
    if (setStatusFromDbusError(btReply)) {
        return;
    }

    const auto btArgs = btReply.arguments();
    if (btArgs.size() < 6) {
        m_statusText = QStringLiteral("Unexpected GetBackendTasksSettings reply");
        Q_EMIT statusTextChanged();
        return;
    }

    m_btHasSeparateLlm = btArgs[0].toBool();
    m_btLlmConnector = btArgs[1].toString();
    m_btLlmModel = btArgs[2].toString();
    m_btLlmBaseUrl = btArgs[3].toString();
    m_btDreamingEnabled = btArgs[4].toBool();
    m_btDreamingIntervalSecs = static_cast<int>(btArgs[5].toULongLong());
    m_btArchiveAfterDays = btArgs.size() > 6 ? static_cast<int>(btArgs[6].toUInt()) : 0;

    QDBusMessage wsAuthReply = iface.call("GetWsAuthSettings");
    if (!setStatusFromDbusError(wsAuthReply)) {
        const auto wsAuthArgs = wsAuthReply.arguments();
        if (wsAuthArgs.size() >= 6) {
            m_wsAuthMethods = wsAuthArgs[0].toStringList();
            m_oidcIssuer = wsAuthArgs[1].toString();
            m_oidcAuthEndpoint = wsAuthArgs[2].toString();
            m_oidcTokenEndpoint = wsAuthArgs[3].toString();
            m_oidcClientId = wsAuthArgs[4].toString();
            m_oidcScopes = wsAuthArgs[5].toString();
            if (m_oidcScopes.isEmpty()) {
                m_oidcScopes = QStringLiteral("openid profile email");
            }
        }
    }

    loadWidgetConnectionSettings();

    // Probe the voice service + read its config so the Voice tab is populated
    // on open. This emits voiceChanged/voiceConfigChanged itself.
    loadVoiceSettings();

    Q_EMIT connectorChanged();
    Q_EMIT modelChanged();
    Q_EMIT baseUrlChanged();
    Q_EMIT embConnectorChanged();
    Q_EMIT embModelChanged();
    Q_EMIT embBaseUrlChanged();
    Q_EMIT embHasApiKeyChanged();
    Q_EMIT embAvailableChanged();
    Q_EMIT embIsDefaultChanged();
    Q_EMIT hasApiKeyChanged();
    Q_EMIT apiKeyInputChanged();
    Q_EMIT gitEnabledChanged();
    Q_EMIT gitRemoteUrlChanged();
    Q_EMIT gitRemoteNameChanged();
    Q_EMIT gitPushOnUpdateChanged();
    Q_EMIT dbUrlChanged();
    Q_EMIT dbMaxConnectionsChanged();
    Q_EMIT connectionNamesChanged();
    Q_EMIT defaultConnectionNameChanged();
    emitConnectionSelectionChanged();
    Q_EMIT btDreamingEnabledChanged();
    Q_EMIT btDreamingIntervalSecsChanged();
    Q_EMIT btArchiveAfterDaysChanged();
    Q_EMIT btHasSeparateLlmChanged();
    Q_EMIT btLlmConnectorChanged();
    Q_EMIT btLlmModelChanged();
    Q_EMIT btLlmBaseUrlChanged();
    Q_EMIT hostedToolSearchChanged();
    Q_EMIT hostedToolSearchAvailableChanged();
    Q_EMIT wsAuthMethodsChanged();
    Q_EMIT oidcIssuerChanged();
    Q_EMIT oidcAuthEndpointChanged();
    Q_EMIT oidcTokenEndpointChanged();
    Q_EMIT oidcClientIdChanged();
    Q_EMIT oidcScopesChanged();

    m_statusText = QStringLiteral("Loaded settings from desktop-assistant daemon");
    Q_EMIT statusTextChanged();
    setNeedsSave(false);
}

void DesktopAssistantKcm::save()
{
    // Immediate-save throughout: each setter has already pushed its
    // change via the corresponding D-Bus method, so save() is a no-op.
    // Kept for KQuickConfigModule's vtable / for any future hooks.
}

void DesktopAssistantKcm::pushPersistenceSettings()
{
    QDBusInterface iface(SERVICE, PATH, IFACE, QDBusConnection::sessionBus());
    QDBusMessage reply = iface.call(
        "SetPersistenceSettings",
        m_gitEnabled,
        m_gitRemoteUrl,
        m_gitRemoteName,
        m_gitPushOnUpdate
    );
    setStatusFromDbusError(reply);
}

void DesktopAssistantKcm::pushDatabaseSettings()
{
    QDBusInterface iface(SERVICE, PATH, IFACE, QDBusConnection::sessionBus());
    QDBusMessage reply = iface.call(
        "SetDatabaseSettings",
        m_dbUrl,
        static_cast<uint>(m_dbMaxConnections)
    );
    setStatusFromDbusError(reply);
}

void DesktopAssistantKcm::pushBackendTasksSettings()
{
    QDBusInterface iface(SERVICE, PATH, IFACE, QDBusConnection::sessionBus());
    QDBusMessage reply = iface.call(
        "SetBackendTasksSettings",
        m_btLlmConnector,
        m_btLlmModel,
        m_btLlmBaseUrl,
        m_btDreamingEnabled,
        static_cast<qulonglong>(m_btDreamingIntervalSecs),
        static_cast<uint>(m_btArchiveAfterDays)
    );
    setStatusFromDbusError(reply);
}

void DesktopAssistantKcm::pushWsAuthSettings()
{
    QDBusInterface iface(SERVICE, PATH, IFACE, QDBusConnection::sessionBus());
    QDBusMessage reply = iface.call(
        "SetWsAuthSettings",
        m_wsAuthMethods,
        m_oidcIssuer,
        m_oidcAuthEndpoint,
        m_oidcTokenEndpoint,
        m_oidcClientId,
        m_oidcScopes
    );
    setStatusFromDbusError(reply);
}

void DesktopAssistantKcm::defaults()
{
    applyChatDefaults();
    applySearchDefaults();
    applyBackendDefaults();
    setApiKeyInput(QString());
    m_statusText = QStringLiteral("Applied connector defaults");
    Q_EMIT statusTextChanged();
}

void DesktopAssistantKcm::applyChatDefaults()
{
    QDBusInterface iface(SERVICE, PATH, IFACE, QDBusConnection::sessionBus());
    const auto llmConnector = normalizeConnector(m_connector);

    ConnectorDefaults defaults;
    QString errorText;
    if (!fetchConnectorDefaults(iface, llmConnector, &defaults, &errorText)) {
        m_statusText = errorText;
        Q_EMIT statusTextChanged();
        return;
    }

    setModel(defaults.llmModel);
    setBaseUrl(defaults.llmBaseUrl);
    if (m_hostedToolSearchAvailable != defaults.hostedToolSearchAvailable) {
        m_hostedToolSearchAvailable = defaults.hostedToolSearchAvailable;
        Q_EMIT hostedToolSearchAvailableChanged();
    }
}

void DesktopAssistantKcm::applySearchDefaults()
{
    QDBusInterface iface(SERVICE, PATH, IFACE, QDBusConnection::sessionBus());
    auto embeddingConnector = normalizeConnector(m_embConnector.isEmpty() ? m_connector : m_embConnector);

    ConnectorDefaults defaults;
    QString errorText;
    if (!fetchConnectorDefaults(iface, embeddingConnector, &defaults, &errorText)) {
        m_statusText = errorText;
        Q_EMIT statusTextChanged();
        return;
    }

    if (!defaults.embeddingsAvailable) {
        embeddingConnector = QStringLiteral("openai");
        if (m_embConnector == QLatin1String("anthropic")) {
            setEmbConnector(embeddingConnector);
        }

        if (!fetchConnectorDefaults(iface, embeddingConnector, &defaults, &errorText)) {
            m_statusText = errorText;
            Q_EMIT statusTextChanged();
            return;
        }
    }

    setEmbModel(defaults.embeddingsModel);
    setEmbBaseUrl(defaults.embeddingsBaseUrl);
}

void DesktopAssistantKcm::applyBackendDefaults()
{
    QDBusInterface iface(SERVICE, PATH, IFACE, QDBusConnection::sessionBus());
    const auto btConnector = normalizeConnector(m_btLlmConnector.isEmpty() ? m_connector : m_btLlmConnector);

    ConnectorDefaults defaults;
    QString errorText;
    if (!fetchConnectorDefaults(iface, btConnector, &defaults, &errorText)) {
        m_statusText = errorText;
        Q_EMIT statusTextChanged();
        return;
    }

    if (m_btLlmConnector.isEmpty()) {
        setBtLlmConnector(btConnector);
    }
    setBtLlmModel(defaults.backendLlmModel);
    setBtLlmBaseUrl(defaults.llmBaseUrl);
}

void DesktopAssistantKcm::addRemoteConnection(const QString &name)
{
    const auto normalized = normalizeConnectionName(name);
    if (normalized.isEmpty()) {
        m_statusText = QStringLiteral("Connection name is required");
        Q_EMIT statusTextChanged();
        return;
    }

    static const QRegularExpression validName(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));
    if (!validName.match(normalized).hasMatch()) {
        m_statusText = QStringLiteral("Connection name may include letters, numbers, dot, underscore, and dash");
        Q_EMIT statusTextChanged();
        return;
    }

    const auto existing = connectionIndexByName(normalized);
    if (existing >= 0) {
        setSelectedConnectionByIndex(existing);
        m_statusText = QStringLiteral("Connection already exists");
        Q_EMIT statusTextChanged();
        return;
    }

    ConnectionProfile connection;
    connection.name = normalized;
    connection.transport = QStringLiteral("ws");
    connection.wsUrl = QString::fromUtf8(DEFAULT_WS_URL);
    connection.wsSubject = QString::fromUtf8(DEFAULT_WS_SUBJECT);
    m_connections.push_back(connection);

    Q_EMIT connectionNamesChanged();
    setSelectedConnectionByIndex(m_connections.size() - 1);
    m_statusText = QStringLiteral("Added connection '%1'").arg(normalized);
    Q_EMIT statusTextChanged();
    saveWidgetConnectionSettings();
}

void DesktopAssistantKcm::removeSelectedConnection()
{
    const auto index = selectedConnectionIndex();
    if (index < 0) {
        return;
    }

    if (m_connections.size() <= 1) {
        m_statusText = QStringLiteral("At least one connection is required");
        Q_EMIT statusTextChanged();
        return;
    }

    const auto name = m_connections[index].name;
    m_connections.removeAt(index);
    if (m_defaultConnectionName == name) {
        const auto localIndex = connectionIndexByName(QString::fromUtf8(DEFAULT_CONNECTION_NAME));
        m_defaultConnectionName = localIndex >= 0
            ? QString::fromUtf8(DEFAULT_CONNECTION_NAME)
            : m_connections.front().name;
        Q_EMIT defaultConnectionNameChanged();
    }

    Q_EMIT connectionNamesChanged();
    setSelectedConnectionName(m_defaultConnectionName);
    m_statusText = QStringLiteral("Removed connection '%1'").arg(name);
    Q_EMIT statusTextChanged();
    saveWidgetConnectionSettings();
}

bool DesktopAssistantKcm::setStatusFromDbusError(const QDBusMessage &message)
{
    if (message.type() != QDBusMessage::ErrorMessage) {
        return false;
    }

    m_statusText = message.errorMessage();
    if (m_statusText.isEmpty()) {
        m_statusText = QStringLiteral("D-Bus call failed");
    }
    Q_EMIT statusTextChanged();
    return true;
}

int DesktopAssistantKcm::connectionIndexByName(const QString &name) const
{
    const auto normalized = normalizeConnectionName(name);
    for (qsizetype i = 0; i < m_connections.size(); ++i) {
        if (m_connections[i].name == normalized) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int DesktopAssistantKcm::selectedConnectionIndex() const
{
    return connectionIndexByName(m_selectedConnectionName);
}

void DesktopAssistantKcm::loadWidgetConnectionSettings()
{
    m_connections.clear();
    m_defaultConnectionName = QString::fromUtf8(DEFAULT_CONNECTION_NAME);
    m_selectedConnectionName = QString::fromUtf8(DEFAULT_CONNECTION_NAME);

    QString localDbusService = QString::fromUtf8(SERVICE);
    QString legacyTransport;
    QString legacyWsUrl;
    QString legacyWsSubject;
    QString configuredDefaultConnection;

    QFile file(widgetSettingsPath());
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();

        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            const auto root = doc.object();

            localDbusService = root.value(QStringLiteral("dbus_service")).toString().trimmed();
            if (localDbusService.isEmpty()) {
                localDbusService = QString::fromUtf8(SERVICE);
            }

            legacyTransport = root.value(QStringLiteral("transport")).toString().trimmed().toLower();
            legacyWsUrl = root.value(QStringLiteral("ws_url")).toString().trimmed();
            legacyWsSubject = root.value(QStringLiteral("ws_subject")).toString().trimmed();
            configuredDefaultConnection = normalizeConnectionName(
                root.value(QStringLiteral("default_connection")).toString()
            );

            const auto rawConnections = root.value(QStringLiteral("connections"));
            if (rawConnections.isArray()) {
                const auto array = rawConnections.toArray();
                for (const auto &item : array) {
                    if (!item.isObject()) {
                        continue;
                    }
                    const auto obj = item.toObject();
                    ConnectionProfile connection;
                    connection.name = normalizeConnectionName(obj.value(QStringLiteral("name")).toString());
                    if (connection.name.isEmpty() || connectionIndexByName(connection.name) >= 0) {
                        continue;
                    }

                    connection.transport = obj.value(QStringLiteral("transport")).toString().trimmed().toLower();
                    if (connection.transport == QLatin1String("dbus")) {
                        connection.transport = QStringLiteral("dbus");
                    } else if (connection.transport == QLatin1String("ws")) {
                        connection.transport = QStringLiteral("ws");
                    } else if (connection.name == QLatin1String(DEFAULT_CONNECTION_NAME)) {
                        connection.transport = QStringLiteral("dbus");
                    } else {
                        connection.transport = QStringLiteral("ws");
                    }

                    connection.dbusService = obj.value(QStringLiteral("dbus_service")).toString().trimmed();
                    if (connection.transport == QLatin1String("dbus") && connection.dbusService.isEmpty()) {
                        connection.dbusService = QString::fromUtf8(SERVICE);
                    }

                    connection.wsUrl = obj.value(QStringLiteral("ws_url")).toString().trimmed();
                    connection.wsSubject = obj.value(QStringLiteral("ws_subject")).toString().trimmed();
                    if (connection.transport == QLatin1String("ws")) {
                        if (connection.wsUrl.isEmpty()) {
                            connection.wsUrl = QString::fromUtf8(DEFAULT_WS_URL);
                        }
                        if (connection.wsSubject.isEmpty()) {
                            connection.wsSubject = QString::fromUtf8(DEFAULT_WS_SUBJECT);
                        }
                    }

                    m_connections.push_back(connection);
                }
            }
        }
    }

    if (m_connections.isEmpty()) {
        ConnectionProfile localConnection;
        localConnection.name = QString::fromUtf8(DEFAULT_CONNECTION_NAME);
        localConnection.transport = QStringLiteral("dbus");
        localConnection.dbusService = localDbusService;
        m_connections.push_back(localConnection);

        const auto useLegacyWs = legacyTransport == QLatin1String("ws") || !legacyWsUrl.isEmpty();
        if (useLegacyWs) {
            ConnectionProfile legacyConnection;
            legacyConnection.name = QStringLiteral("legacy-ws");
            legacyConnection.transport = QStringLiteral("ws");
            legacyConnection.wsUrl = legacyWsUrl.isEmpty() ? QString::fromUtf8(DEFAULT_WS_URL) : legacyWsUrl;
            legacyConnection.wsSubject = legacyWsSubject.isEmpty() ? QString::fromUtf8(DEFAULT_WS_SUBJECT) : legacyWsSubject;
            m_connections.push_back(legacyConnection);
            m_defaultConnectionName = QStringLiteral("legacy-ws");
        }

        if (legacyTransport == QLatin1String("dbus")) {
            m_defaultConnectionName = QString::fromUtf8(DEFAULT_CONNECTION_NAME);
        }
    }

    if (!configuredDefaultConnection.isEmpty() && connectionIndexByName(configuredDefaultConnection) >= 0) {
        m_defaultConnectionName = configuredDefaultConnection;
    }

    if (connectionIndexByName(m_defaultConnectionName) < 0) {
        const auto localIndex = connectionIndexByName(QString::fromUtf8(DEFAULT_CONNECTION_NAME));
        if (localIndex >= 0) {
            m_defaultConnectionName = QString::fromUtf8(DEFAULT_CONNECTION_NAME);
        } else if (!m_connections.isEmpty()) {
            m_defaultConnectionName = m_connections.front().name;
        } else {
            m_defaultConnectionName = QString::fromUtf8(DEFAULT_CONNECTION_NAME);
        }
    }
    m_selectedConnectionName = m_defaultConnectionName;
}

bool DesktopAssistantKcm::saveWidgetConnectionSettings()
{
    if (connectionIndexByName(m_defaultConnectionName) < 0) {
        const auto localIndex = connectionIndexByName(QString::fromUtf8(DEFAULT_CONNECTION_NAME));
        if (localIndex >= 0) {
            m_defaultConnectionName = QString::fromUtf8(DEFAULT_CONNECTION_NAME);
        } else if (!m_connections.isEmpty()) {
            m_defaultConnectionName = m_connections.front().name;
        } else {
            m_defaultConnectionName = QString::fromUtf8(DEFAULT_CONNECTION_NAME);
        }
    }

    QJsonObject root;
    QFile existing(widgetSettingsPath());
    if (existing.exists() && existing.open(QIODevice::ReadOnly)) {
        const auto existingDoc = QJsonDocument::fromJson(existing.readAll());
        if (existingDoc.isObject()) {
            root = existingDoc.object();
        }
        existing.close();
    }

    QJsonArray connections;
    for (const auto &connection : m_connections) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), connection.name);
        item.insert(QStringLiteral("transport"), connection.transport);
        if (connection.transport == QLatin1String("dbus")) {
            item.insert(QStringLiteral("dbus_service"), connection.dbusService.isEmpty() ? QString::fromUtf8(SERVICE) : connection.dbusService);
        } else {
            item.insert(QStringLiteral("ws_url"), connection.wsUrl.isEmpty() ? QString::fromUtf8(DEFAULT_WS_URL) : connection.wsUrl);
            item.insert(QStringLiteral("ws_subject"), connection.wsSubject.isEmpty() ? QString::fromUtf8(DEFAULT_WS_SUBJECT) : connection.wsSubject);
        }
        connections.push_back(item);
    }

    root.insert(QStringLiteral("connections"), connections);
    root.insert(QStringLiteral("default_connection"), m_defaultConnectionName);

    const auto localIndex = connectionIndexByName(QString::fromUtf8(DEFAULT_CONNECTION_NAME));
    if (localIndex >= 0 && m_connections[localIndex].transport == QLatin1String("dbus")) {
        root.insert(
            QStringLiteral("dbus_service"),
            m_connections[localIndex].dbusService.isEmpty() ? QString::fromUtf8(SERVICE) : m_connections[localIndex].dbusService
        );
    } else {
        const auto firstDbus = std::find_if(m_connections.begin(), m_connections.end(), [](const ConnectionProfile &connection) {
            return connection.transport == QLatin1String("dbus");
        });
        if (firstDbus != m_connections.end()) {
            root.insert(
                QStringLiteral("dbus_service"),
                firstDbus->dbusService.isEmpty() ? QString::fromUtf8(SERVICE) : firstDbus->dbusService
            );
        }
    }

    const auto defaultIndex = connectionIndexByName(m_defaultConnectionName);
    if (defaultIndex >= 0 && m_connections[defaultIndex].transport == QLatin1String("ws")) {
        root.insert(QStringLiteral("transport"), QStringLiteral("ws"));
        root.insert(QStringLiteral("ws_url"), m_connections[defaultIndex].wsUrl);
        root.insert(QStringLiteral("ws_subject"), m_connections[defaultIndex].wsSubject);
    } else {
        root.insert(QStringLiteral("transport"), QStringLiteral("dbus"));
    }

    QFile file(widgetSettingsPath());
    const auto fileInfo = QFileInfo(file);
    QDir dir;
    if (!dir.mkpath(fileInfo.absolutePath())) {
        m_statusText = QStringLiteral("Unable to create widget settings directory");
        Q_EMIT statusTextChanged();
        return false;
    }

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_statusText = QStringLiteral("Unable to write widget settings file");
        Q_EMIT statusTextChanged();
        return false;
    }

    const QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

void DesktopAssistantKcm::setSelectedConnectionByIndex(int index)
{
    if (index < 0 || index >= m_connections.size()) {
        return;
    }

    const auto nextName = m_connections[index].name;
    if (m_selectedConnectionName == nextName) {
        return;
    }

    m_selectedConnectionName = nextName;
    Q_EMIT selectedConnectionNameChanged();
    emitConnectionSelectionChanged();
}

void DesktopAssistantKcm::emitConnectionSelectionChanged()
{
    Q_EMIT selectedConnectionTransportChanged();
    Q_EMIT selectedConnectionDbusServiceChanged();
    Q_EMIT selectedConnectionWsUrlChanged();
    Q_EMIT selectedConnectionWsSubjectChanged();
    Q_EMIT selectedConnectionRemovableChanged();
}

// === Voice page (adele-kde#30) ==============================================

namespace {
// Documented recommended defaults the "Reset to defaults" buttons restore
// (adele-kde#37). These are the values the issue documents as the good
// starting point; they can differ from the daemon's absent-key fallbacks
// (e.g. silence/follow-up) where the recommended UX value is more forgiving.
constexpr double kVoiceDefaultSensitivity = 0.45;
constexpr double kVoiceDefaultSpeechThreshold = 0.5;
constexpr int kVoiceDefaultSilenceDurationMs = 3000;
constexpr int kVoiceDefaultFollowupTimeoutMs = 10000;
constexpr bool kVoiceDefaultWakeEager = false;

// Format a double as a TOML float literal that ALWAYS carries a decimal point.
// The daemon's f32 config fields (wake_word.sensitivity, vad.speech_threshold)
// are parsed by the `toml` crate, which does NOT coerce a bare integer into a
// float field — so emitting "1" or "0" for a slider at its extreme would make
// the whole config fail to parse. Guarantee a fractional part (e.g. "1.0").
QString formatTomlFloat(double value)
{
    QString s = QString::number(value, 'g', 4);
    if (!s.contains(QLatin1Char('.')) && !s.contains(QLatin1Char('e'))
        && !s.contains(QLatin1Char('E'))) {
        s += QStringLiteral(".0");
    }
    return s;
}
}

QString DesktopAssistantKcm::voiceConfigPath()
{
    // The voice daemon reads ~/.config/adele-voice/config.toml (XDG config).
    const auto configHome = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return QDir(configHome).filePath(QStringLiteral("adele-voice/config.toml"));
}

bool DesktopAssistantKcm::probeVoiceAvailable() const
{
    // NameHasOwner on the session bus — cheap, and (unlike constructing a
    // QDBusInterface) it does NOT D-Bus-activate the service, so a masked /
    // uninstalled daemon reports false instead of being spawned.
    QDBusInterface dbusDaemon(
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"),
        QDBusConnection::sessionBus()
    );
    if (!dbusDaemon.isValid()) {
        return false;
    }
    QDBusReply<bool> reply = dbusDaemon.call(QStringLiteral("NameHasOwner"), QString::fromUtf8(VOICE_SERVICE));
    return reply.isValid() && reply.value();
}

QString DesktopAssistantKcm::runSystemctlUser(const QStringList &args, bool *ok) const
{
    if (ok != nullptr) {
        *ok = false;
    }
    QProcess proc;
    proc.start(QStringLiteral("systemctl"), QStringList{QStringLiteral("--user")} + args);
    if (!proc.waitForStarted(3000)) {
        return QString();
    }
    if (!proc.waitForFinished(5000)) {
        proc.kill();
        proc.waitForFinished(1000);
        return QString();
    }
    const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    if (ok != nullptr) {
        *ok = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    }
    return out;
}

int DesktopAssistantKcm::probeVoiceAutostart() const
{
    // `systemctl --user is-enabled adele-voice.service`:
    //   "enabled"  -> 1 (starts at login)
    //   "disabled"/"masked"/anything else -> 0
    //   unit not installed (non-zero, "not-found"/empty) -> -1 (unknown)
    bool ok = false;
    const QString state = runSystemctlUser(
        QStringList{QStringLiteral("is-enabled"), QString::fromUtf8(VOICE_UNIT)}, &ok);
    if (state == QLatin1String("enabled") || state == QLatin1String("enabled-runtime")) {
        return 1;
    }
    if (state == QLatin1String("disabled") || state == QLatin1String("masked")
        || state == QLatin1String("masked-runtime") || state == QLatin1String("static")) {
        return 0;
    }
    // "not-found", empty, or any unexpected token -> treat as not-installed.
    return -1;
}

void DesktopAssistantKcm::readVoiceConfig()
{
    // Reset to the daemon's documented defaults, then overlay whatever the
    // config file specifies. We only care about a handful of scalar keys under
    // [audio], [wake_word], [stt], and [tts]; everything else is left untouched
    // on write (see writeVoiceConfig). Defaults track the daemon's
    // SttConfig/TtsConfig::default() (repo adelie-ai/voice).
    m_sttLanguage = QStringLiteral("en");
    m_sttModelPath.clear();
    m_wakeSensitivity = 0.5;
    m_inputDevice = QStringLiteral("default");
    m_outputDevice = QStringLiteral("default");
    // Tuning knobs (adele-kde#37): absent-key fallbacks mirror the daemon's
    // VadConfig/AssistantConfig::default() so the page shows what the daemon
    // would actually use, not the (more forgiving) Reset-button values.
    m_vadSpeechThreshold = 0.5;
    m_vadSilenceDurationMs = 800;
    m_followupTimeoutMs = 8000;
    m_wakeEager = false;
    m_listeningCue.clear();
    m_ttsBackend = QStringLiteral("kokoro");
    m_kokoroLang = QStringLiteral("en-us");
    m_piperModelPath.clear();
    m_pollyEngine = QStringLiteral("neural");
    m_pollyRegion.clear();

    QFile file(voiceConfigPath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QString currentSection;
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
            currentSection = trimmed.mid(1, trimmed.size() - 2).trimmed();
            continue;
        }
        const int eq = trimmed.indexOf(QLatin1Char('='));
        if (eq < 0) {
            continue;
        }
        const QString key = trimmed.left(eq).trimmed();
        QString value = trimmed.mid(eq + 1).trimmed();
        // Strip surrounding double quotes from string values.
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) {
            value = value.mid(1, value.size() - 2);
        }

        if (currentSection == QLatin1String("audio")) {
            if (key == QLatin1String("input_device")) {
                m_inputDevice = value;
            } else if (key == QLatin1String("output_device")) {
                m_outputDevice = value;
            }
        } else if (currentSection == QLatin1String("wake_word")) {
            if (key == QLatin1String("sensitivity")) {
                bool parsed = false;
                const double d = value.toDouble(&parsed);
                if (parsed) {
                    m_wakeSensitivity = d;
                }
            } else if (key == QLatin1String("eager")) {
                m_wakeEager = value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
            } else if (key == QLatin1String("listening_cue")) {
                m_listeningCue = value;
            }
        } else if (currentSection == QLatin1String("vad")) {
            if (key == QLatin1String("speech_threshold")) {
                bool parsed = false;
                const double d = value.toDouble(&parsed);
                if (parsed) {
                    m_vadSpeechThreshold = d;
                }
            } else if (key == QLatin1String("silence_duration_ms")) {
                bool parsed = false;
                const int n = value.toInt(&parsed);
                if (parsed) {
                    m_vadSilenceDurationMs = n;
                }
            }
        } else if (currentSection == QLatin1String("assistant")) {
            if (key == QLatin1String("followup_timeout_ms")) {
                bool parsed = false;
                const int n = value.toInt(&parsed);
                if (parsed) {
                    m_followupTimeoutMs = n;
                }
            }
        } else if (currentSection == QLatin1String("stt")) {
            if (key == QLatin1String("language")) {
                m_sttLanguage = value;
            } else if (key == QLatin1String("model_path")) {
                m_sttModelPath = value;
            }
        } else if (currentSection == QLatin1String("tts")) {
            if (key == QLatin1String("backend")) {
                m_ttsBackend = value;
            } else if (key == QLatin1String("kokoro_lang")) {
                m_kokoroLang = value;
            } else if (key == QLatin1String("model_path")) {
                // [tts].model_path is the Piper voice model (distinct from
                // [stt].model_path, the Whisper model).
                m_piperModelPath = value;
            } else if (key == QLatin1String("polly_engine")) {
                m_pollyEngine = value;
            } else if (key == QLatin1String("polly_region")) {
                m_pollyRegion = value;
            }
        }
    }
    file.close();
}

bool DesktopAssistantKcm::writeVoiceConfig()
{
    // Surgical, section-aware merge: rewrite only the keys we own, preserving
    // every other line (comments, unknown keys, sections we don't manage) so
    // the user's hand-tuned config survives a settings change from this page.
    //
    // Some keys are "omit when empty": the daemon types them as optional
    // (polly_region) or computes a default path (model paths) when absent, so
    // writing an empty string would override that default with a broken value.
    // For those, an empty value means "drop the key" — we neither replace nor
    // append it, and we delete any existing line so clearing the field in the
    // GUI restores the daemon default.
    const QString path = voiceConfigPath();

    QStringList lines;
    {
        QFile in(path);
        if (in.exists() && in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stream(&in);
            while (!stream.atEnd()) {
                lines.push_back(stream.readLine());
            }
            in.close();
        }
    }

    struct Target {
        QString section;
        QString key;
        QString value; // raw string (formatLine adds quotes) or bare number
        bool quoted;
        bool omitWhenEmpty = false; // empty value -> drop the key entirely
        bool done = false;
    };
    QVector<Target> targets = {
        {QStringLiteral("audio"), QStringLiteral("input_device"), m_inputDevice, true, false, false},
        {QStringLiteral("audio"), QStringLiteral("output_device"), m_outputDevice, true, false, false},
        {QStringLiteral("wake_word"), QStringLiteral("sensitivity"),
         formatTomlFloat(m_wakeSensitivity), false, false, false},
        // Forward-compat wake-word keys (voice#50/#51). `eager` is a bare bool;
        // `listening_cue` is omit-when-empty so an unset cue doesn't pin a value.
        {QStringLiteral("wake_word"), QStringLiteral("eager"),
         m_wakeEager ? QStringLiteral("true") : QStringLiteral("false"), false, false, false},
        {QStringLiteral("wake_word"), QStringLiteral("listening_cue"), m_listeningCue, true, true, false},
        // Endpointing (adele-kde#37): [vad] + [assistant].
        {QStringLiteral("vad"), QStringLiteral("speech_threshold"),
         formatTomlFloat(m_vadSpeechThreshold), false, false, false},
        {QStringLiteral("vad"), QStringLiteral("silence_duration_ms"),
         QString::number(m_vadSilenceDurationMs), false, false, false},
        {QStringLiteral("assistant"), QStringLiteral("followup_timeout_ms"),
         QString::number(m_followupTimeoutMs), false, false, false},
        {QStringLiteral("stt"), QStringLiteral("language"), m_sttLanguage, true, false, false},
        {QStringLiteral("stt"), QStringLiteral("model_path"), m_sttModelPath, true, true, false},
        {QStringLiteral("tts"), QStringLiteral("backend"), m_ttsBackend, true, false, false},
        {QStringLiteral("tts"), QStringLiteral("kokoro_lang"), m_kokoroLang, true, false, false},
        {QStringLiteral("tts"), QStringLiteral("model_path"), m_piperModelPath, true, true, false},
        {QStringLiteral("tts"), QStringLiteral("polly_engine"), m_pollyEngine, true, false, false},
        {QStringLiteral("tts"), QStringLiteral("polly_region"), m_pollyRegion, true, true, false},
    };

    auto formatLine = [](const Target &t) -> QString {
        const QString rhs = t.quoted ? (QLatin1Char('"') + t.value + QLatin1Char('"')) : t.value;
        return t.key + QStringLiteral(" = ") + rhs;
    };
    // Whether a target should actually be written. An omit-when-empty target
    // with no value is dropped (not emitted, and its existing line removed).
    auto shouldEmit = [](const Target &t) -> bool {
        return !(t.omitWhenEmpty && t.value.isEmpty());
    };

    // First pass: update existing keys in place within their section. For a
    // dropped (omit-when-empty, blank) target we mark it done and skip the line
    // so the key disappears from the file.
    QString currentSection;
    QStringList merged;
    for (const QString &raw : lines) {
        const QString trimmed = raw.trimmed();
        if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
            currentSection = trimmed.mid(1, trimmed.size() - 2).trimmed();
            merged.push_back(raw);
            continue;
        }
        bool replaced = false;
        const int eq = trimmed.indexOf(QLatin1Char('='));
        if (eq > 0 && !trimmed.startsWith(QLatin1Char('#'))) {
            const QString key = trimmed.left(eq).trimmed();
            for (auto &t : targets) {
                if (!t.done && currentSection == t.section && key == t.key) {
                    if (shouldEmit(t)) {
                        merged.push_back(formatLine(t));
                    }
                    t.done = true;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            merged.push_back(raw);
        }
    }

    // Second pass: append any keys whose section exists but lacked the key, or
    // whose section is missing entirely. Group appends by section so we emit a
    // section header at most once.
    auto sectionPresent = [&merged](const QString &section) -> bool {
        const QString header = QStringLiteral("[") + section + QStringLiteral("]");
        for (const QString &l : merged) {
            if (l.trimmed() == header) {
                return true;
            }
        }
        return false;
    };

    // A target still needs appending only if it's not done AND it should emit.
    // Mark dropped (omit-when-empty, blank) targets done up front so they
    // neither force a section to be created nor get appended.
    for (auto &t : targets) {
        if (!t.done && !shouldEmit(t)) {
            t.done = true;
        }
    }

    // Collect remaining (not-done) targets grouped by section, preserving order.
    QStringList sectionsNeedingAppend;
    for (const auto &t : targets) {
        if (!t.done && !sectionsNeedingAppend.contains(t.section)) {
            sectionsNeedingAppend.push_back(t.section);
        }
    }
    for (const QString &section : sectionsNeedingAppend) {
        if (sectionPresent(section)) {
            // Insert the missing key(s) right after the existing header.
            const QString header = QStringLiteral("[") + section + QStringLiteral("]");
            QStringList rebuilt;
            for (const QString &l : merged) {
                rebuilt.push_back(l);
                if (l.trimmed() == header) {
                    for (auto &t : targets) {
                        if (!t.done && t.section == section) {
                            rebuilt.push_back(formatLine(t));
                            t.done = true;
                        }
                    }
                }
            }
            merged = rebuilt;
        } else {
            // Append a fresh section at the end.
            if (!merged.isEmpty() && !merged.last().trimmed().isEmpty()) {
                merged.push_back(QString());
            }
            merged.push_back(QStringLiteral("[") + section + QStringLiteral("]"));
            for (auto &t : targets) {
                if (!t.done && t.section == section) {
                    merged.push_back(formatLine(t));
                    t.done = true;
                }
            }
        }
    }

    QFileInfo fileInfo(path);
    QDir dir;
    if (!dir.mkpath(fileInfo.absolutePath())) {
        m_statusText = QStringLiteral("Unable to create voice config directory");
        Q_EMIT statusTextChanged();
        return false;
    }
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_statusText = QStringLiteral("Unable to write voice config file");
        Q_EMIT statusTextChanged();
        return false;
    }
    QTextStream stream(&out);
    for (const QString &l : merged) {
        stream << l << '\n';
    }
    out.close();
    return true;
}

bool DesktopAssistantKcm::voiceServiceAvailable() const
{
    return m_voiceServiceAvailable;
}

bool DesktopAssistantKcm::voiceEnabled() const
{
    return m_voiceEnabled;
}

void DesktopAssistantKcm::setVoiceEnabled(bool value)
{
    if (m_voiceEnabled == value) {
        return;
    }
    if (!m_voiceServiceAvailable) {
        // No live daemon to toggle — reflect the request but don't pretend it
        // took. The next loadVoiceSettings() reconciles.
        return;
    }
    QDBusInterface iface(VOICE_SERVICE, VOICE_PATH, VOICE_IFACE, QDBusConnection::sessionBus());
    QDBusMessage reply = iface.call(QStringLiteral("SetEnabled"), value);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        m_statusText = reply.errorMessage().isEmpty()
            ? QStringLiteral("Failed to toggle voice")
            : reply.errorMessage();
        Q_EMIT statusTextChanged();
        return;
    }
    m_voiceEnabled = value;
    m_statusText = value ? QStringLiteral("“Hey Adele” enabled") : QStringLiteral("“Hey Adele” disabled");
    Q_EMIT voiceChanged();
    Q_EMIT statusTextChanged();
}

QVariantList DesktopAssistantKcm::voiceList() const
{
    return m_voiceList;
}

QString DesktopAssistantKcm::voiceCurrentId() const
{
    return m_voiceCurrentId;
}

int DesktopAssistantKcm::voiceCurrentSpeaker() const
{
    return m_voiceCurrentSpeaker;
}

int DesktopAssistantKcm::voiceAutostart() const
{
    return m_voiceAutostart;
}

QString DesktopAssistantKcm::sttLanguage() const
{
    return m_sttLanguage;
}

void DesktopAssistantKcm::setSttLanguage(const QString &value)
{
    const QString normalized = value.trimmed();
    if (m_sttLanguage == normalized) {
        return;
    }
    m_sttLanguage = normalized;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

QString DesktopAssistantKcm::sttModelPath() const
{
    return m_sttModelPath;
}

void DesktopAssistantKcm::setSttModelPath(const QString &value)
{
    const QString normalized = value.trimmed();
    if (m_sttModelPath == normalized) {
        return;
    }
    m_sttModelPath = normalized;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

QString DesktopAssistantKcm::ttsBackend() const
{
    return m_ttsBackend;
}

void DesktopAssistantKcm::setTtsBackend(const QString &value)
{
    const QString normalized = value.trimmed();
    // Constrain to the backends the daemon knows; ignore anything else so a
    // stray binding can't write a bogus backend into the config.
    if (normalized != QLatin1String("kokoro") && normalized != QLatin1String("piper")
        && normalized != QLatin1String("polly")) {
        return;
    }
    if (m_ttsBackend == normalized) {
        return;
    }
    m_ttsBackend = normalized;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

QString DesktopAssistantKcm::kokoroLang() const
{
    return m_kokoroLang;
}

void DesktopAssistantKcm::setKokoroLang(const QString &value)
{
    const QString normalized = value.trimmed();
    if (m_kokoroLang == normalized) {
        return;
    }
    m_kokoroLang = normalized;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

QString DesktopAssistantKcm::piperModelPath() const
{
    return m_piperModelPath;
}

void DesktopAssistantKcm::setPiperModelPath(const QString &value)
{
    const QString normalized = value.trimmed();
    if (m_piperModelPath == normalized) {
        return;
    }
    m_piperModelPath = normalized;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

QString DesktopAssistantKcm::pollyEngine() const
{
    return m_pollyEngine;
}

void DesktopAssistantKcm::setPollyEngine(const QString &value)
{
    const QString normalized = value.trimmed();
    // The GUI offers neural / generative; the daemon also accepts long-form /
    // standard. Accept any non-empty token so a hand-edited config round-trips,
    // but ignore empty so we never blank out the daemon default.
    if (normalized.isEmpty() || m_pollyEngine == normalized) {
        return;
    }
    m_pollyEngine = normalized;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

QString DesktopAssistantKcm::pollyRegion() const
{
    return m_pollyRegion;
}

void DesktopAssistantKcm::setPollyRegion(const QString &value)
{
    const QString normalized = value.trimmed();
    if (m_pollyRegion == normalized) {
        return;
    }
    m_pollyRegion = normalized;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

double DesktopAssistantKcm::wakeSensitivity() const
{
    return m_wakeSensitivity;
}

void DesktopAssistantKcm::setWakeSensitivity(double value)
{
    // Rustpotter sensitivity is a 0..1 confidence threshold.
    const double clamped = std::clamp(value, 0.0, 1.0);
    if (qFuzzyCompare(m_wakeSensitivity + 1.0, clamped + 1.0)) {
        return;
    }
    m_wakeSensitivity = clamped;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

QString DesktopAssistantKcm::inputDevice() const
{
    return m_inputDevice;
}

void DesktopAssistantKcm::setInputDevice(const QString &value)
{
    const QString normalized = value.trimmed().isEmpty() ? QStringLiteral("default") : value.trimmed();
    if (m_inputDevice == normalized) {
        return;
    }
    m_inputDevice = normalized;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

QString DesktopAssistantKcm::outputDevice() const
{
    return m_outputDevice;
}

void DesktopAssistantKcm::setOutputDevice(const QString &value)
{
    const QString normalized = value.trimmed().isEmpty() ? QStringLiteral("default") : value.trimmed();
    if (m_outputDevice == normalized) {
        return;
    }
    m_outputDevice = normalized;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

double DesktopAssistantKcm::vadSpeechThreshold() const
{
    return m_vadSpeechThreshold;
}

void DesktopAssistantKcm::setVadSpeechThreshold(double value)
{
    // Silero VAD speech probability threshold (0..1).
    const double clamped = std::clamp(value, 0.0, 1.0);
    if (qFuzzyCompare(m_vadSpeechThreshold + 1.0, clamped + 1.0)) {
        return;
    }
    m_vadSpeechThreshold = clamped;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

int DesktopAssistantKcm::vadSilenceDurationMs() const
{
    return m_vadSilenceDurationMs;
}

void DesktopAssistantKcm::setVadSilenceDurationMs(int value)
{
    const int clamped = std::clamp(value, 0, 20000);
    if (m_vadSilenceDurationMs == clamped) {
        return;
    }
    m_vadSilenceDurationMs = clamped;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

int DesktopAssistantKcm::followupTimeoutMs() const
{
    return m_followupTimeoutMs;
}

void DesktopAssistantKcm::setFollowupTimeoutMs(int value)
{
    const int clamped = std::clamp(value, 0, 60000);
    if (m_followupTimeoutMs == clamped) {
        return;
    }
    m_followupTimeoutMs = clamped;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

bool DesktopAssistantKcm::wakeEager() const
{
    return m_wakeEager;
}

void DesktopAssistantKcm::setWakeEager(bool value)
{
    if (m_wakeEager == value) {
        return;
    }
    m_wakeEager = value;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

QString DesktopAssistantKcm::listeningCue() const
{
    return m_listeningCue;
}

void DesktopAssistantKcm::setListeningCue(const QString &value)
{
    const QString normalized = value.trimmed();
    if (m_listeningCue == normalized) {
        return;
    }
    m_listeningCue = normalized;
    Q_EMIT voiceConfigChanged();
    writeVoiceConfig();
}

QVariantList DesktopAssistantKcm::inputDeviceOptions() const
{
    return m_inputDeviceOptions;
}

QVariantList DesktopAssistantKcm::outputDeviceOptions() const
{
    return m_outputDeviceOptions;
}

void DesktopAssistantKcm::loadVoiceSettings()
{
    m_voiceServiceAvailable = probeVoiceAvailable();

    if (m_voiceServiceAvailable) {
        QDBusInterface iface(VOICE_SERVICE, VOICE_PATH, VOICE_IFACE, QDBusConnection::sessionBus());

        QDBusReply<bool> enabledReply = iface.call(QStringLiteral("GetEnabled"));
        if (enabledReply.isValid()) {
            m_voiceEnabled = enabledReply.value();
        }

        // ListVoices -> a(sssu). Marshal into a QVariantList of maps the QML
        // page reads by key (voice_id / display_name / language / num_speakers).
        m_voiceList.clear();
        QDBusMessage voicesReply = iface.call(QStringLiteral("ListVoices"));
        if (voicesReply.type() != QDBusMessage::ErrorMessage && !voicesReply.arguments().isEmpty()) {
            const QDBusArgument arg = voicesReply.arguments().first().value<QDBusArgument>();
            arg.beginArray();
            while (!arg.atEnd()) {
                arg.beginStructure();
                QString id;
                QString name;
                QString lang;
                uint speakers = 0;
                arg >> id >> name >> lang >> speakers;
                arg.endStructure();
                QVariantMap entry;
                entry.insert(QStringLiteral("voice_id"), id);
                entry.insert(QStringLiteral("display_name"), name);
                entry.insert(QStringLiteral("language"), lang);
                entry.insert(QStringLiteral("num_speakers"), static_cast<int>(speakers));
                m_voiceList.push_back(entry);
            }
            arg.endArray();
        }

        // GetVoice -> (si): (voice_id, speaker_id); speaker_id -1 if unset.
        QDBusMessage currentReply = iface.call(QStringLiteral("GetVoice"));
        if (currentReply.type() != QDBusMessage::ErrorMessage) {
            const auto args = currentReply.arguments();
            if (args.size() >= 2) {
                m_voiceCurrentId = args[0].toString();
                m_voiceCurrentSpeaker = args[1].toInt();
            } else if (args.size() == 1) {
                // Some bindings wrap the struct; unpack via QDBusArgument.
                const QDBusArgument inner = args.first().value<QDBusArgument>();
                inner.beginStructure();
                inner >> m_voiceCurrentId >> m_voiceCurrentSpeaker;
                inner.endStructure();
            }
        }
    } else {
        m_voiceEnabled = false;
        m_voiceList.clear();
        m_voiceCurrentId.clear();
        m_voiceCurrentSpeaker = -1;
    }

    m_voiceAutostart = probeVoiceAutostart();
    readVoiceConfig();
    // NB: device enumeration (pactl + arecord/aplay subprocesses) is NOT done
    // here — loadVoiceSettings() runs from the KCM constructor's load(), which
    // fires for every tab of this settings module, so spawning audio tools then
    // would add startup latency even for users who never open the Voice tab. The
    // Voice page calls loadAudioDevices() itself from Component.onCompleted.

    Q_EMIT voiceChanged();
    Q_EMIT voiceConfigChanged();
}

void DesktopAssistantKcm::setVoice(const QString &voiceId, int speaker)
{
    if (!m_voiceServiceAvailable) {
        return;
    }
    const QString id = voiceId.trimmed();
    if (id.isEmpty()) {
        return;
    }
    QDBusInterface iface(VOICE_SERVICE, VOICE_PATH, VOICE_IFACE, QDBusConnection::sessionBus());
    QDBusMessage reply = iface.call(QStringLiteral("SetVoice"), id, speaker);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        m_statusText = reply.errorMessage().isEmpty()
            ? QStringLiteral("Failed to set voice")
            : reply.errorMessage();
        Q_EMIT statusTextChanged();
        return;
    }
    m_voiceCurrentId = id;
    m_voiceCurrentSpeaker = speaker;
    m_statusText = QStringLiteral("Voice set to %1").arg(id);
    Q_EMIT voiceChanged();
    Q_EMIT statusTextChanged();
}

void DesktopAssistantKcm::setVoiceAutostart(bool enabled)
{
    if (m_voiceAutostart < 0) {
        // Unit isn't installed — nothing to enable/disable.
        m_statusText = QStringLiteral("Voice service unit is not installed");
        Q_EMIT statusTextChanged();
        return;
    }
    bool ok = false;
    runSystemctlUser(
        QStringList{enabled ? QStringLiteral("enable") : QStringLiteral("disable"),
                    QString::fromUtf8(VOICE_UNIT)},
        &ok);
    if (!ok) {
        m_statusText = enabled
            ? QStringLiteral("Failed to enable voice autostart")
            : QStringLiteral("Failed to disable voice autostart");
        Q_EMIT statusTextChanged();
    } else {
        m_statusText = enabled
            ? QStringLiteral("Voice will start at login")
            : QStringLiteral("Voice autostart disabled");
        Q_EMIT statusTextChanged();
    }
    // Re-probe so the toggle reflects the real unit state (enable can be
    // refused, e.g. for a static unit).
    m_voiceAutostart = probeVoiceAutostart();
    Q_EMIT voiceChanged();
}

void DesktopAssistantKcm::restartVoiceService()
{
    // Config-file settings (TTS backend + per-backend keys, STT, devices,
    // sensitivity) only take effect on (re)start, so this applies them without
    // leaving the page. `restart` starts the unit if it was stopped, which is
    // the behaviour we want from a "Restart voice service" button.
    bool ok = false;
    runSystemctlUser(
        QStringList{QStringLiteral("restart"), QString::fromUtf8(VOICE_UNIT)}, &ok);
    m_statusText = ok ? QStringLiteral("Voice service restarted")
                      : QStringLiteral("Failed to restart the voice service");
    Q_EMIT statusTextChanged();
    // The restart re-spawns the daemon and re-reads config; reconcile the page
    // (availability, enabled, voice list, autostart) against the new process.
    loadVoiceSettings();
}

bool DesktopAssistantKcm::tryDaemonReload() const
{
    // Forward-compatible hot-reload (adele-kde#37). The daemon will expose a
    // `Reload` method on org.desktopAssistant.Voice (voice#52) that re-reads
    // config.toml without a restart. Until that lands the call comes back as
    // UnknownMethod, which we treat as "not supported" -> false so the caller
    // falls back to a service restart. We only attempt this when the service is
    // actually on the bus (constructing the interface would otherwise D-Bus
    // *activate* the daemon, which we don't want from a probe).
    if (!m_voiceServiceAvailable) {
        return false;
    }
    QDBusInterface iface(VOICE_SERVICE, VOICE_PATH, VOICE_IFACE, QDBusConnection::sessionBus());
    if (!iface.isValid()) {
        return false;
    }
    QDBusMessage reply = iface.call(QStringLiteral("Reload"));
    return reply.type() != QDBusMessage::ErrorMessage;
}

void DesktopAssistantKcm::applyVoiceChanges()
{
    // Persist anything still pending, then apply live. Each setter already
    // writes config.toml, but call it once more so an Apply press after a
    // programmatic change (e.g. a Reset) is always durable before we reload.
    writeVoiceConfig();

    if (tryDaemonReload()) {
        m_statusText = QStringLiteral("Voice settings reloaded");
        Q_EMIT statusTextChanged();
        // A reload doesn't change availability/voice list, but re-read config so
        // the page reflects exactly what's now on disk.
        loadVoiceSettings();
        return;
    }

    // Fall back to a restart. restartVoiceService() sets its own status and
    // re-reads live state. If the unit isn't installed there's nothing to do
    // beyond the on-disk write we already performed; say so honestly.
    if (m_voiceAutostart < 0 && !m_voiceServiceAvailable) {
        m_statusText = QStringLiteral(
            "Saved. The voice service isn't running; changes apply when it next starts.");
        Q_EMIT statusTextChanged();
        return;
    }
    restartVoiceService();
}

QVariantList DesktopAssistantKcm::enumerateAudioDevices(const QString &direction) const
{
    // The voice daemon resolves a configured device by SUBSTRING-matching the
    // cpal (ALSA host) device name (repo adelie-ai/voice,
    // crates/audio-cpal/src/{source,sink}.rs: `desc.name().contains(name)`).
    // So the stored value must be a substring of a cpal device name. The most
    // reliable such substring is the ALSA card id (the `CARD=<id>` token), which
    // appears in cpal names like `sysdefault:CARD=<id>` / `front:CARD=<id>`.
    //
    // We enumerate card ids from `arecord -L` / `aplay -L` (the same ALSA PCM
    // list cpal-alsa walks) and, when available, dress each with a friendly
    // description from `pactl` matched on the card id. This keeps the stored
    // value cpal-matchable while the label stays human-readable.
    const bool isInput = direction == QLatin1String("input");

    // Friendly labels keyed by ALSA card id, harvested from pactl descriptions.
    QHash<QString, QString> cardLabels;
    {
        QProcess pactl;
        pactl.start(QStringLiteral("pactl"),
                    QStringList{QStringLiteral("list"), isInput ? QStringLiteral("sources") : QStringLiteral("sinks")});
        if (pactl.waitForStarted(2000) && pactl.waitForFinished(3000)
            && pactl.exitStatus() == QProcess::NormalExit) {
            const QString out = QString::fromUtf8(pactl.readAllStandardOutput());
            QString pendingDesc;
            const QStringList plines = out.split(QLatin1Char('\n'));
            // pactl groups properties per device, each block starting with a
            // "Source #N" / "Sink #N" header. Reset the pending Description on
            // each header so a device with no alsa.card_name can't leak its
            // Description onto the next device's card (the value we store is
            // always the correct CARD token; only this label could mismatch).
            static const QRegularExpression deviceHeader(
                QStringLiteral("^(Source|Sink) #\\d+"));
            static const QRegularExpression cardProp(
                QStringLiteral("alsa\\.card_name\\s*=\\s*\"([^\"]+)\""));
            static const QRegularExpression descProp(QStringLiteral("^\\s*Description:\\s*(.+)$"));
            for (const QString &pl : plines) {
                if (deviceHeader.match(pl).hasMatch()) {
                    pendingDesc.clear();
                    continue;
                }
                const auto descMatch = descProp.match(pl);
                if (descMatch.hasMatch()) {
                    pendingDesc = descMatch.captured(1).trimmed();
                    continue;
                }
                const auto cardMatch = cardProp.match(pl);
                if (cardMatch.hasMatch()) {
                    const QString card = cardMatch.captured(1).trimmed();
                    if (!card.isEmpty() && !pendingDesc.isEmpty()) {
                        cardLabels.insert(card, pendingDesc);
                    }
                }
            }
        }
    }

    QVariantList out;
    QSet<QString> seen;
    QProcess alsa;
    alsa.start(isInput ? QStringLiteral("arecord") : QStringLiteral("aplay"),
               QStringList{QStringLiteral("-L")});
    if (alsa.waitForStarted(2000) && alsa.waitForFinished(3000)
        && alsa.exitStatus() == QProcess::NormalExit) {
        const QString text = QString::fromUtf8(alsa.readAllStandardOutput());
        const QStringList lines = text.split(QLatin1Char('\n'));

        // Well-known virtual PCMs that route through the sound server. These are
        // valid cpal device names and, on a PipeWire/Pulse box, usually the
        // *right* choice (they follow the server's default and resample) — the
        // CARD=<id> entries below bypass the server and pin a raw card. We offer
        // them as their own rows so the picker isn't card-only (the previous
        // free-text field let users type these; ALSA card names like "PCH"
        // can't reach the server-managed default device). Order matters: list
        // these first so the recommended server routes are near the top.
        static const QStringList kVirtualPcms = {
            QStringLiteral("pipewire"), QStringLiteral("pulse"), QStringLiteral("jack")};
        static const QHash<QString, QString> kVirtualLabels = {
            {QStringLiteral("pipewire"), QStringLiteral("PipeWire (follows the sound server)")},
            {QStringLiteral("pulse"), QStringLiteral("PulseAudio (follows the sound server)")},
            {QStringLiteral("jack"), QStringLiteral("JACK")},
        };
        for (const QString &pcm : kVirtualPcms) {
            // Match the bare top-level PCM line exactly (e.g. "pipewire"), not a
            // CARD= entry that happens to contain the word.
            if (lines.contains(pcm) && !seen.contains(pcm)) {
                seen.insert(pcm);
                QVariantMap entry;
                entry.insert(QStringLiteral("value"), pcm);
                entry.insert(QStringLiteral("label"), kVirtualLabels.value(pcm, pcm));
                out.push_back(entry);
            }
        }

        // Raw ALSA cards (cpal substring-matches the CARD=<id> token).
        static const QRegularExpression cardToken(QStringLiteral("CARD=([A-Za-z0-9_]+)"));
        for (const QString &line : lines) {
            const auto m = cardToken.match(line);
            if (!m.hasMatch()) {
                continue;
            }
            const QString card = m.captured(1);
            if (card.isEmpty() || seen.contains(card)) {
                continue;
            }
            seen.insert(card);
            // Prefer a pactl-sourced friendly name keyed by card *name* (which
            // often differs from the short card id); fall back to the card id.
            QString label = card;
            for (auto it = cardLabels.constBegin(); it != cardLabels.constEnd(); ++it) {
                if (it.key().contains(card, Qt::CaseInsensitive)
                    || card.contains(it.key(), Qt::CaseInsensitive)) {
                    label = it.value();
                    break;
                }
            }
            QVariantMap entry;
            entry.insert(QStringLiteral("value"), card);
            entry.insert(QStringLiteral("label"),
                         label == card ? QStringLiteral("Card: %1").arg(card) : label);
            out.push_back(entry);
        }
    }
    return out;
}

void DesktopAssistantKcm::loadAudioDevices()
{
    auto withDefault = [](const QVariantList &devices) -> QVariantList {
        QVariantList list;
        QVariantMap def;
        def.insert(QStringLiteral("value"), QStringLiteral("default"));
        def.insert(QStringLiteral("label"), QStringLiteral("Follow system default (recommended)"));
        list.push_back(def);
        list.append(devices);
        return list;
    };

    m_inputDeviceOptions = withDefault(enumerateAudioDevices(QStringLiteral("input")));
    m_outputDeviceOptions = withDefault(enumerateAudioDevices(QStringLiteral("output")));

    // A hand-edited config can point at a device that didn't enumerate (a
    // headset that's currently unplugged, say). Keep the selection visible
    // instead of silently snapping it to "default": append it as its own row.
    auto ensurePresent = [](QVariantList &list, const QString &value) {
        if (value.isEmpty() || value == QLatin1String("default")) {
            return;
        }
        for (const QVariant &v : list) {
            if (v.toMap().value(QStringLiteral("value")).toString() == value) {
                return;
            }
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("value"), value);
        entry.insert(QStringLiteral("label"), QStringLiteral("%1 (configured)").arg(value));
        list.push_back(entry);
    };
    ensurePresent(m_inputDeviceOptions, m_inputDevice);
    ensurePresent(m_outputDeviceOptions, m_outputDevice);

    Q_EMIT audioDevicesChanged();
}

double DesktopAssistantKcm::measureInputLevel()
{
    // Briefly sample the input device and report a 0..1 peak so the page can
    // nudge a too-quiet mic (ties into voice#47). We use `pactl` to find the
    // running source's monitor isn't reliable for input, so instead we read the
    // source volume peak via a short `parecord` capture and compute the peak
    // sample magnitude. Returns -1 when no level could be taken.
    if (QStandardPaths::findExecutable(QStringLiteral("parecord")).isEmpty()) {
        return -1.0;
    }

    // Raw 16-bit mono so we can scan samples directly; a short capture is enough
    // to catch speech without making the button feel laggy. We let
    // PulseAudio/PipeWire pick the default source rather than trying to resolve
    // the cpal-style stored value to a pactl source name (the mapping is
    // substring-only and unreliable) — the default source is still a useful
    // gauge of whether the mic is producing audible signal.
    const QStringList args = {
        QStringLiteral("--raw"),
        QStringLiteral("--format=s16le"),
        QStringLiteral("--rate=16000"),
        QStringLiteral("--channels=1"),
    };

    QProcess rec;
    rec.start(QStringLiteral("parecord"), args);
    if (!rec.waitForStarted(2000)) {
        return -1.0;
    }
    // Let it capture briefly, then stop and read what we got.
    rec.waitForReadyRead(400);
    QByteArray data = rec.readAllStandardOutput();
    rec.terminate();
    if (!rec.waitForFinished(1000)) {
        rec.kill();
        rec.waitForFinished(500);
    }
    data += rec.readAllStandardOutput();
    if (data.size() < 2) {
        return -1.0;
    }

    qint16 peak = 0;
    const auto *samples = reinterpret_cast<const qint16 *>(data.constData());
    const int count = data.size() / static_cast<int>(sizeof(qint16));
    for (int i = 0; i < count; ++i) {
        const qint16 s = samples[i];
        const int mag = s < 0 ? -static_cast<int>(s) : static_cast<int>(s);
        if (mag > peak) {
            peak = static_cast<qint16>(qMin(mag, 32767));
        }
    }
    return static_cast<double>(peak) / 32767.0;
}

void DesktopAssistantKcm::resetWakeDefaults()
{
    m_wakeSensitivity = kVoiceDefaultSensitivity;
    m_wakeEager = kVoiceDefaultWakeEager;
    m_listeningCue.clear();
    writeVoiceConfig();
    Q_EMIT voiceConfigChanged();
    m_statusText = QStringLiteral("Wake-word settings reset to defaults");
    Q_EMIT statusTextChanged();
}

void DesktopAssistantKcm::resetEndpointingDefaults()
{
    m_vadSpeechThreshold = kVoiceDefaultSpeechThreshold;
    m_vadSilenceDurationMs = kVoiceDefaultSilenceDurationMs;
    m_followupTimeoutMs = kVoiceDefaultFollowupTimeoutMs;
    writeVoiceConfig();
    Q_EMIT voiceConfigChanged();
    m_statusText = QStringLiteral("Endpointing settings reset to defaults");
    Q_EMIT statusTextChanged();
}

void DesktopAssistantKcm::resetDeviceDefaults()
{
    m_inputDevice = QStringLiteral("default");
    m_outputDevice = QStringLiteral("default");
    writeVoiceConfig();
    Q_EMIT voiceConfigChanged();
    m_statusText = QStringLiteral("Audio devices set to follow the system default");
    Q_EMIT statusTextChanged();
}

void DesktopAssistantKcm::resetVoiceTuningDefaults()
{
    m_wakeSensitivity = kVoiceDefaultSensitivity;
    m_wakeEager = kVoiceDefaultWakeEager;
    m_listeningCue.clear();
    m_vadSpeechThreshold = kVoiceDefaultSpeechThreshold;
    m_vadSilenceDurationMs = kVoiceDefaultSilenceDurationMs;
    m_followupTimeoutMs = kVoiceDefaultFollowupTimeoutMs;
    m_inputDevice = QStringLiteral("default");
    m_outputDevice = QStringLiteral("default");
    writeVoiceConfig();
    Q_EMIT voiceConfigChanged();
    m_statusText = QStringLiteral("Voice tuning reset to defaults");
    Q_EMIT statusTextChanged();
}

void DesktopAssistantKcm::daemonCall(const QString &command, const QJSValue &payload, const QJSValue &callback)
{
    // Dispatches multi-connection commands through the daemon's D-Bus
    // surface (org.desktopAssistant.Connections). The KCM is a local-only
    // client, so D-Bus is the right transport — the matching WebSocket
    // path was used briefly during the multi-connection rollout but
    // required a per-call JWT + a TLS handshake against the daemon's
    // self-signed CA, which is wasted work for a session-bus client.

    auto fail = [&](const QString &message) {
        if (callback.isCallable()) {
            QJSEngine *engine = qjsEngine(this);
            QJSValueList argv;
            argv << QJSValue(QJSValue::NullValue);
            argv << (engine ? engine->toScriptValue(message) : QJSValue(message));
            QJSValue cb = callback;
            cb.call(argv);
        }
    };

    // Normalise the command name to snake_case so QML callers can use either
    // form (`list_connections` or `ListConnections`).
    QString snake;
    for (QChar c : command.trimmed()) {
        if (c.isUpper()) {
            if (!snake.isEmpty()) snake.append(QChar('_'));
            snake.append(c.toLower());
        } else {
            snake.append(c);
        }
    }
    if (snake.isEmpty()) {
        fail(QStringLiteral("daemonCall: missing command variant"));
        return;
    }

    QJsonObject payloadObj;
    if (payload.isObject()) {
        const QJsonValue asJson = QJsonValue::fromVariant(payload.toVariant());
        if (asJson.isObject()) {
            payloadObj = asJson.toObject();
        }
    }

    auto serializePayloadField = [&payloadObj](const QString &key) -> QString {
        const QJsonValue value = payloadObj.value(key);
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    };

    // Knowledge management commands route to the dedicated
    // `org.desktopAssistant.Knowledge` interface on a different object
    // path (#73). The Connections interface stays as before.
    const bool isKnowledge = snake.startsWith(QLatin1String("list_knowledge_"))
        || snake.startsWith(QLatin1String("get_knowledge_"))
        || snake.startsWith(QLatin1String("search_knowledge_"))
        || snake.startsWith(QLatin1String("create_knowledge_"))
        || snake.startsWith(QLatin1String("update_knowledge_"))
        || snake.startsWith(QLatin1String("delete_knowledge_"));

    const QString objectPath = isKnowledge
        ? QStringLiteral("/org/desktopAssistant/Knowledge")
        : QStringLiteral("/org/desktopAssistant/Connections");
    const QString interfaceName = isKnowledge
        ? QStringLiteral("org.desktopAssistant.Knowledge")
        : QStringLiteral("org.desktopAssistant.Connections");

    QDBusInterface iface(
        QStringLiteral("org.desktopAssistant"),
        objectPath,
        interfaceName,
        QDBusConnection::sessionBus()
    );
    if (!iface.isValid()) {
        fail(QStringLiteral("Daemon is not running on the session bus"));
        return;
    }

    auto serializeArrayField = [&payloadObj](const QString &key) -> QString {
        const QJsonValue value = payloadObj.value(key);
        if (value.isNull() || value.isUndefined()) {
            return QStringLiteral("null");
        }
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    };
    auto serializeValueField = [&payloadObj](const QString &key) -> QString {
        const QJsonValue value = payloadObj.value(key);
        if (value.isNull() || value.isUndefined()) {
            return QString();
        }
        return QString::fromUtf8(
            QJsonDocument::fromVariant(value.toVariant()).toJson(QJsonDocument::Compact));
    };

    // Build the D-Bus call argument list per command. Methods that return a
    // JSON-encoded `CommandResult` produce a string we re-parse below; the
    // `Ack` commands return an empty signature.
    QDBusMessage reply;
    bool returnsJson = false;
    if (snake == QLatin1String("list_connections")) {
        reply = iface.call(QStringLiteral("ListConnections"));
        returnsJson = true;
    } else if (snake == QLatin1String("get_purposes")) {
        reply = iface.call(QStringLiteral("GetPurposes"));
        returnsJson = true;
    } else if (snake == QLatin1String("list_available_models")) {
        const QString cid = payloadObj.value(QStringLiteral("connection_id")).toString();
        const bool refresh = payloadObj.value(QStringLiteral("refresh")).toBool(false);
        reply = iface.call(QStringLiteral("ListAvailableModels"), cid, refresh);
        returnsJson = true;
    } else if (snake == QLatin1String("create_connection")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        const QString configJson = serializePayloadField(QStringLiteral("config"));
        reply = iface.call(QStringLiteral("CreateConnection"), id, configJson);
    } else if (snake == QLatin1String("update_connection")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        const QString configJson = serializePayloadField(QStringLiteral("config"));
        reply = iface.call(QStringLiteral("UpdateConnection"), id, configJson);
    } else if (snake == QLatin1String("delete_connection")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        const bool force = payloadObj.value(QStringLiteral("force")).toBool(false);
        reply = iface.call(QStringLiteral("DeleteConnection"), id, force);
    } else if (snake == QLatin1String("set_purpose")) {
        const QString purpose = payloadObj.value(QStringLiteral("purpose")).toString();
        const QString configJson = serializePayloadField(QStringLiteral("config"));
        reply = iface.call(QStringLiteral("SetPurpose"), purpose, configJson);
    } else if (snake == QLatin1String("list_knowledge_entries")) {
        const uint limit = static_cast<uint>(
            payloadObj.value(QStringLiteral("limit")).toInt(50));
        const uint offset = static_cast<uint>(
            payloadObj.value(QStringLiteral("offset")).toInt(0));
        const QString tagFilterJson = serializeArrayField(QStringLiteral("tag_filter"));
        reply = iface.call(QStringLiteral("ListEntries"), limit, offset, tagFilterJson);
        returnsJson = true;
    } else if (snake == QLatin1String("get_knowledge_entry")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        reply = iface.call(QStringLiteral("GetEntry"), id);
        returnsJson = true;
    } else if (snake == QLatin1String("search_knowledge_entries")) {
        const QString query = payloadObj.value(QStringLiteral("query")).toString();
        const QString tagFilterJson = serializeArrayField(QStringLiteral("tag_filter"));
        const uint limit = static_cast<uint>(
            payloadObj.value(QStringLiteral("limit")).toInt(50));
        reply = iface.call(QStringLiteral("SearchEntries"), query, tagFilterJson, limit);
        returnsJson = true;
    } else if (snake == QLatin1String("create_knowledge_entry")) {
        const QString content = payloadObj.value(QStringLiteral("content")).toString();
        const QString tagsJson = serializeArrayField(QStringLiteral("tags"));
        const QString metadataJson = serializeValueField(QStringLiteral("metadata"));
        reply = iface.call(QStringLiteral("CreateEntry"), content, tagsJson, metadataJson);
        returnsJson = true;
    } else if (snake == QLatin1String("update_knowledge_entry")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        const QString content = payloadObj.value(QStringLiteral("content")).toString();
        const QString tagsJson = serializeArrayField(QStringLiteral("tags"));
        const QString metadataJson = serializeValueField(QStringLiteral("metadata"));
        reply = iface.call(QStringLiteral("UpdateEntry"), id, content, tagsJson, metadataJson);
        returnsJson = true;
    } else if (snake == QLatin1String("delete_knowledge_entry")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        reply = iface.call(QStringLiteral("DeleteEntry"), id);
    } else {
        fail(QStringLiteral("daemonCall: unsupported command '%1'").arg(snake));
        return;
    }

    if (reply.type() == QDBusMessage::ErrorMessage) {
        fail(reply.errorMessage().isEmpty() ? QStringLiteral("D-Bus call failed") : reply.errorMessage());
        return;
    }

    QVariant resultVariant;
    if (returnsJson) {
        const auto args = reply.arguments();
        if (args.isEmpty()) {
            fail(QStringLiteral("D-Bus reply missing JSON payload"));
            return;
        }
        const QString json = args.first().toString();
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            fail(QStringLiteral("Failed to parse daemon reply: %1").arg(parseError.errorString()));
            return;
        }
        resultVariant = doc.toVariant();
    }

    if (callback.isCallable()) {
        QJSEngine *engine = qjsEngine(this);
        QJSValue resultValue = engine
            ? engine->toScriptValue(resultVariant)
            : QJSValue(QJSValue::NullValue);
        QJSValueList argv;
        argv << resultValue;
        argv << QJSValue(QJSValue::NullValue);
        QJSValue cb = callback;
        cb.call(argv);
    }
}

#include "desktopassistantkcm.moc"
