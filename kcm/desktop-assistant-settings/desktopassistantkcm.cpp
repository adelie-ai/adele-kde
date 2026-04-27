#include "desktopassistantkcm.h"

#include <algorithm>
#include <dlfcn.h>
#include <sys/stat.h>
#include <QDateTime>

#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <KPluginFactory>

namespace {
constexpr auto SERVICE = "org.desktopAssistant";
constexpr auto PATH = "/org/desktopAssistant/Settings";
constexpr auto IFACE = "org.desktopAssistant.Settings";
constexpr auto DEFAULT_CONNECTION_NAME = "local";
constexpr auto DEFAULT_WS_URL = "ws://127.0.0.1:11339/ws";
constexpr auto DEFAULT_WS_SUBJECT = "desktop-widget";

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

void DesktopAssistantKcm::wsCall(const QString &command, const QJSValue &payload, const QJSValue &callback)
{
    // Despite the legacy "ws" name, this dispatches the multi-connection
    // commands through the daemon's D-Bus surface
    // (org.desktopAssistant.Connections), not WebSocket. The WS path used to
    // require a fresh JWT for every call and a TLS handshake against a
    // self-signed CA; routing through D-Bus removes both problems.

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
        fail(QStringLiteral("wsCall: missing command variant"));
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

    QDBusInterface iface(
        QStringLiteral("org.desktopAssistant"),
        QStringLiteral("/org/desktopAssistant/Connections"),
        QStringLiteral("org.desktopAssistant.Connections"),
        QDBusConnection::sessionBus()
    );
    if (!iface.isValid()) {
        fail(QStringLiteral("Daemon is not running on the session bus"));
        return;
    }

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
    } else {
        fail(QStringLiteral("wsCall: unsupported command '%1'").arg(snake));
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
