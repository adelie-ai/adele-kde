#include "desktopassistantkcm.h"

#include "daemonreply.h"
#include "voiceconfig.h"

#include <algorithm>
#include <memory>
#include <dlfcn.h>
#include <sys/stat.h>
#include <QDateTime>

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
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

// Async D-Bus call timeouts (KDE-2 / #57). Every daemon round-trip is bounded
// so a wedged daemon can never hang the System Settings UI thread. The default
// is generous enough for ordinary local Set*/Get* round-trips; model
// enumeration may hit a remote provider, and knowledge search may scan a large
// store, so those get longer budgets.
constexpr int DBUS_TIMEOUT_DEFAULT_MS = 5000;
constexpr int DBUS_TIMEOUT_MODELS_MS = 30000;
constexpr int DBUS_TIMEOUT_SEARCH_MS = 15000;

QString widgetSettingsPath()
{
    const auto configHome = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return QDir(configHome).filePath(QStringLiteral("desktop-assistant/widget_settings.json"));
}

QString normalizeConnectionName(const QString &name)
{
    return name.trimmed();
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

    // Live refresh (#dream-cycle): relay the daemon's
    // `org.desktopAssistant.Knowledge.EntriesChanged` D-Bus signal — emitted
    // when an entry is created/updated/deleted on any client or a maintenance
    // pass rewrites entries — to the `knowledgeEntriesChanged()` Qt signal the
    // QML Knowledge page connects to, so its list refreshes in place. The signal
    // carries no args, so we relay straight to the Qt signal (no slot needed).
    QDBusConnection::sessionBus().connect(
        QString::fromUtf8(SERVICE),
        QStringLiteral("/org/desktopAssistant/Knowledge"),
        QStringLiteral("org.desktopAssistant.Knowledge"),
        QStringLiteral("EntriesChanged"),
        this,
        SIGNAL(knowledgeEntriesChanged()));
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

// --- Personality (adele-kde#42) ---------------------------------------------
// Trait values persist on the daemon's aggregate config (GetConfig/SetConfig on
// org.desktopAssistant.Settings) as per-trait u32 fields appended to ConfigData
// / ConfigPatchArgs (desktop-assistant#226). There are no granular
// get/set_personality D-Bus methods. load() reads the trailing personality u32s
// from the GetConfig reply by index (QtDBus flattens the returned struct into
// positional args), or keeps the built-in defaults when the daemon predates
// #226; each setter pushes one trait via a SetConfig ConfigPatchArgs struct.
namespace {
// Order of ConfigPatchArgs.set_personality_* / personality_* pairs, matching
// the seven u32 trait fields appended to ConfigData (desktop-assistant#226):
// professionalism, warmth, directness, enthusiasm, humor, sarcasm,
// pretentiousness — the issue #42 display order.
constexpr int PERSONALITY_TRAIT_COUNT = 7;
const char *const PERSONALITY_SET_FIELDS[PERSONALITY_TRAIT_COUNT] = {
    "set_personality_professionalism",
    "set_personality_warmth",
    "set_personality_directness",
    "set_personality_enthusiasm",
    "set_personality_humor",
    "set_personality_sarcasm",
    "set_personality_pretentiousness",
};

// Number of ConfigData fields that precede the personality block (the pre-#226
// struct arity). If GetConfig returns exactly this many the daemon has no
// personality surface yet and we keep the built-in defaults.
constexpr int CONFIG_DATA_BASE_FIELDS = 18;

// D-Bus signature of those 18 leading ConfigData fields, in declaration order
// (desktop-assistant crates/dbus-interface/src/settings.rs `struct ConfigData`):
//   llm_connector(s) llm_model(s) llm_base_url(s) llm_has_api_key(b)
//   embeddings_connector(s) embeddings_model(s) embeddings_base_url(s)
//   embeddings_has_api_key(b) embeddings_available(b) embeddings_is_default(b)
//   persistence_enabled(b) persistence_remote_url(s) persistence_remote_name(s)
//   persistence_push_on_update(b) llm_temperature(d) llm_top_p(d)
//   llm_max_tokens(u) llm_hosted_tool_search(i)
// KDE-8 (#63) validates the GetConfig reply against this signature before
// indexing the trailing personality block, so a daemon field inserted ahead of
// the block is detected (schema mismatch -> keep defaults) instead of silently
// shifting garbage into the 0..4-clamped traits. Must stay in sync with
// ConfigData; its length must equal CONFIG_DATA_BASE_FIELDS (asserted below).
constexpr auto CONFIG_DATA_BASE_SIGNATURE = "sssbsssbbbbssbddui";
static_assert(std::char_traits<char>::length(CONFIG_DATA_BASE_SIGNATURE)
                  == static_cast<size_t>(CONFIG_DATA_BASE_FIELDS),
              "CONFIG_DATA_BASE_SIGNATURE must describe exactly "
              "CONFIG_DATA_BASE_FIELDS leading ConfigData fields");

int clampTrait(int value)
{
    return std::clamp(value, 0, 4);
}
} // namespace

int DesktopAssistantKcm::personalityProfessionalism() const { return m_personalityProfessionalism; }
int DesktopAssistantKcm::personalityWarmth() const { return m_personalityWarmth; }
int DesktopAssistantKcm::personalityDirectness() const { return m_personalityDirectness; }
int DesktopAssistantKcm::personalityEnthusiasm() const { return m_personalityEnthusiasm; }
int DesktopAssistantKcm::personalityHumor() const { return m_personalityHumor; }
int DesktopAssistantKcm::personalitySarcasm() const { return m_personalitySarcasm; }
int DesktopAssistantKcm::personalityPretentiousness() const { return m_personalityPretentiousness; }

void DesktopAssistantKcm::setPersonalityProfessionalism(int value)
{
    setPersonalityTrait(&m_personalityProfessionalism, value, "set_personality_professionalism");
}
void DesktopAssistantKcm::setPersonalityWarmth(int value)
{
    setPersonalityTrait(&m_personalityWarmth, value, "set_personality_warmth");
}
void DesktopAssistantKcm::setPersonalityDirectness(int value)
{
    setPersonalityTrait(&m_personalityDirectness, value, "set_personality_directness");
}
void DesktopAssistantKcm::setPersonalityEnthusiasm(int value)
{
    setPersonalityTrait(&m_personalityEnthusiasm, value, "set_personality_enthusiasm");
}
void DesktopAssistantKcm::setPersonalityHumor(int value)
{
    setPersonalityTrait(&m_personalityHumor, value, "set_personality_humor");
}
void DesktopAssistantKcm::setPersonalitySarcasm(int value)
{
    setPersonalityTrait(&m_personalitySarcasm, value, "set_personality_sarcasm");
}
void DesktopAssistantKcm::setPersonalityPretentiousness(int value)
{
    setPersonalityTrait(&m_personalityPretentiousness, value, "set_personality_pretentiousness");
}

void DesktopAssistantKcm::setPersonalityTrait(int *slot, int value, const char *setField)
{
    const int clamped = clampTrait(value);
    if (*slot == clamped) {
        return;
    }
    *slot = clamped;
    Q_EMIT personalityChanged();
    pushPersonalityTrait(setField, clamped);
}

void DesktopAssistantKcm::pushPersonalityTrait(const char *setField, int value)
{
    // Build a ConfigPatchArgs struct (desktop-assistant#226) with every set_*
    // false except this trait, and SetConfig it. The struct's field order/types
    // are fixed by the daemon's zvariant::Type derive; we marshal positionally.
    // The leading (pre-personality) patch fields mirror the current daemon's
    // input signature (bsbsbsbsbsbsbsbbbsbsbbbdbdbubi); the 7 trailing
    // bool+u32 pairs are the personality block #226 appends.
    //
    // KDE-8 (#63): this patch is hand-encoded POSITIONALLY, so it is only safe
    // when the daemon's config struct matches the shape we assume. load() sets
    // m_personalitySchemaOk once a GetConfig reply has passed signature
    // validation; if it never did (daemon down at load, pre-#226 daemon, or a
    // schema mismatch) the positional patch could write the wrong fields, so we
    // refuse to push and surface why. The in-memory slider value still updated,
    // so the UI reflects the user's choice — it just isn't persisted to a daemon
    // whose schema we can't trust.
    if (!m_personalitySchemaOk) {
        m_statusText = QStringLiteral(
            "Personality not saved: the desktop-assistant daemon's config schema "
            "could not be validated (daemon unavailable or version mismatch).");
        Q_EMIT statusTextChanged();
        return;
    }

    const QString setName = QString::fromLatin1(setField);

    QDBusArgument patch;
    patch.beginStructure();
    // --- LLM string/key fields: set_* = false, value = "" ---
    patch << false << QString();   // set_llm_connector, llm_connector
    patch << false << QString();   // set_llm_model, llm_model
    patch << false << QString();   // set_llm_base_url, llm_base_url
    patch << false << QString();   // set_llm_api_key, llm_api_key
    patch << false << QString();   // set_embeddings_connector, embeddings_connector
    patch << false << QString();   // set_embeddings_model, embeddings_model
    patch << false << QString();   // set_embeddings_base_url, embeddings_base_url
    // --- Persistence ---
    patch << false << false;       // set_persistence_enabled, persistence_enabled
    patch << false << QString();   // set_persistence_remote_url, persistence_remote_url
    patch << false << QString();   // set_persistence_remote_name, persistence_remote_name
    patch << false << false;       // set_persistence_push_on_update, persistence_push_on_update
    // --- LLM numeric sampling ---
    patch << false << 0.0;         // set_llm_temperature, llm_temperature
    patch << false << 0.0;         // set_llm_top_p, llm_top_p
    patch << false << static_cast<uint>(0);  // set_llm_max_tokens, llm_max_tokens
    patch << false << static_cast<int>(0);   // set_llm_hosted_tool_search, llm_hosted_tool_search
    // --- Personality block (#226): 7 x (bool set, u32 value), issue #42 order ---
    for (int i = 0; i < PERSONALITY_TRAIT_COUNT; ++i) {
        const QString field = QString::fromLatin1(PERSONALITY_SET_FIELDS[i]);
        const bool isThis = (field == setName);
        patch << isThis << static_cast<uint>(isThis ? value : 0);
    }
    patch.endStructure();

    // SetConfig takes a single ConfigPatchArgs struct argument. asyncSettingsCall
    // wraps it in a single-element QVariantList, so the struct is passed as one
    // argument (NOT flattened into per-field args). The reply is the flattened
    // ConfigData; we only care whether it errored (KDE-2 / #57, PR 3/5 — was a
    // synchronous QDBusConnection::call that blocked the UI thread). The watcher
    // is parented to `this`, so a reply landing after the KCM is gone is dropped.
    asyncSettingsCall(
        QStringLiteral("SetConfig"), {QVariant::fromValue(patch)}, DBUS_TIMEOUT_DEFAULT_MS,
        [this](const QDBusMessage &reply) {
            // A failure here most likely means the daemon predates the
            // personality fields (desktop-assistant#226) and rejected the longer
            // ConfigPatchArgs signature. Surface it; the in-memory slider value
            // stays so the UI still reflects the user's choice.
            setStatusFromDbusError(reply);
        });
}

void DesktopAssistantKcm::load()
{
    // Bump the load generation (KDE-2 / #57) so any async daemon read issued by
    // this load() that finishes after a *newer* load() has started can detect it
    // was superseded and drop its stale reply — a slow reply from a previous
    // load() must never clobber fresher state.
    const quint64 generation = ++m_loadGeneration;

    // KDE-8 (#63): distrust the daemon's config schema until a GetConfig reply
    // for THIS load() passes signature validation below. Resetting here means a
    // reply that never arrives (daemon down) or is superseded leaves the
    // positional SetConfig push correctly disabled (pushPersonalityTrait guards
    // on this flag).
    m_personalitySchemaOk = false;

    // The legacy single-LLM/embeddings settings (GetLlmSettings /
    // GetEmbeddingsSettings) are no longer surfaced by this KCM: model
    // selection moved to the Connections + Purposes pages (desktop-assistant#17).
    // We therefore skip those reads entirely; the Connections page owns its own
    // load path.
    //
    // Async + fault isolation (KDE-2 #57, preserving KDE-5 #60): each daemon
    // read below is fired in PARALLEL on its own watcher and handled
    // independently. A failing call (daemon down/unreachable, malformed reply)
    // records the first failure into the shared LoadState but must NOT abort the
    // remaining sections — each handler updates only its own fields and emits
    // only its own signals. The purely-local steps below — widget-connection
    // JSON, loadVoiceSettings(), and installing m_voiceWatcher — run IMMEDIATELY,
    // never gated on daemon reachability, so the Voice tab works with the
    // orchestrator down but voice up, and we keep re-probe-on-bus-appearance.
    //
    // Final status is decided once the last of the daemon reads completes
    // (LoadState::pending hits 0): the first recorded daemon error if any,
    // otherwise the clean "Loaded settings" line. Each handler ignores its reply
    // when m_loadGeneration has moved on, so a superseded load()'s completion
    // can neither write fields nor overwrite the newer load()'s status.

    // Shared, refcounted across the per-section handlers for this load() pass.
    // Holds the first daemon error and the count of reads still outstanding.
    struct LoadState {
        QString firstError;
        int pending = 0;
        void recordError(const QString &message) {
            if (firstError.isEmpty()) {
                firstError = message;
            }
        }
    };
    auto state = std::make_shared<LoadState>();

    // Records a section's outcome and, when it is the last to finish, surfaces
    // the aggregate status. Returns false when this reply is stale (a newer
    // load() has started) so the caller skips applying it.
    auto applyOutcome = [this, generation, state](const QString &error) -> bool {
        if (generation != m_loadGeneration) {
            return false; // superseded — drop without touching state/fields
        }
        if (!error.isEmpty()) {
            state->recordError(error);
        }
        if (--state->pending == 0) {
            m_statusText = state->firstError.isEmpty()
                ? QStringLiteral("Loaded settings from desktop-assistant daemon")
                : state->firstError;
            Q_EMIT statusTextChanged();
        }
        return true;
    };

    // --- Parallel daemon reads ----------------------------------------------
    // Count them up front so the "last reply" detection in applyOutcome is
    // correct regardless of completion order.
    state->pending = 5;

    asyncSettingsCall(
        QStringLiteral("GetPersistenceSettings"), {}, DBUS_TIMEOUT_DEFAULT_MS,
        [this, applyOutcome](const QDBusMessage &reply) {
            if (reply.type() == QDBusMessage::ErrorMessage) {
                applyOutcome(dbusErrorMessage(reply));
                return;
            }
            const auto parsed = daemonreply::parsePersistenceReply(reply.arguments());
            if (!applyOutcome(parsed.ok ? QString() : parsed.error)) {
                return;
            }
            if (parsed.ok) {
                m_gitEnabled = parsed.gitEnabled;
                m_gitRemoteUrl = parsed.gitRemoteUrl;
                m_gitRemoteName = parsed.gitRemoteName;
                m_gitPushOnUpdate = parsed.gitPushOnUpdate;
                Q_EMIT gitEnabledChanged();
                Q_EMIT gitRemoteUrlChanged();
                Q_EMIT gitRemoteNameChanged();
                Q_EMIT gitPushOnUpdateChanged();
            }
        });

    asyncSettingsCall(
        QStringLiteral("GetDatabaseSettings"), {}, DBUS_TIMEOUT_DEFAULT_MS,
        [this, applyOutcome](const QDBusMessage &reply) {
            if (reply.type() == QDBusMessage::ErrorMessage) {
                applyOutcome(dbusErrorMessage(reply));
                return;
            }
            const auto parsed = daemonreply::parseDatabaseReply(reply.arguments());
            if (!applyOutcome(parsed.ok ? QString() : parsed.error)) {
                return;
            }
            if (parsed.ok) {
                m_dbUrl = parsed.dbUrl;
                m_dbMaxConnections = parsed.dbMaxConnections;
                Q_EMIT dbUrlChanged();
                Q_EMIT dbMaxConnectionsChanged();
            }
        });

    asyncSettingsCall(
        QStringLiteral("GetBackendTasksSettings"), {}, DBUS_TIMEOUT_DEFAULT_MS,
        [this, applyOutcome](const QDBusMessage &reply) {
            if (reply.type() == QDBusMessage::ErrorMessage) {
                applyOutcome(dbusErrorMessage(reply));
                return;
            }
            const auto parsed = daemonreply::parseBackendTasksReply(reply.arguments());
            if (!applyOutcome(parsed.ok ? QString() : parsed.error)) {
                return;
            }
            if (parsed.ok) {
                // Pass-through LLM fields: stored, echoed back unchanged by
                // pushBackendTasksSettings(); no UI binds them.
                m_btLlmConnector = parsed.llmConnector;
                m_btLlmModel = parsed.llmModel;
                m_btLlmBaseUrl = parsed.llmBaseUrl;
                m_btDreamingEnabled = parsed.dreamingEnabled;
                m_btDreamingIntervalSecs = parsed.dreamingIntervalSecs;
                m_btArchiveAfterDays = parsed.archiveAfterDays;
                Q_EMIT btDreamingEnabledChanged();
                Q_EMIT btDreamingIntervalSecsChanged();
                Q_EMIT btArchiveAfterDaysChanged();
            }
        });

    asyncSettingsCall(
        QStringLiteral("GetWsAuthSettings"), {}, DBUS_TIMEOUT_DEFAULT_MS,
        [this, applyOutcome](const QDBusMessage &reply) {
            if (reply.type() == QDBusMessage::ErrorMessage) {
                applyOutcome(dbusErrorMessage(reply));
                return;
            }
            // WS-auth parse is best-effort: a short reply keeps defaults without
            // recording an error (historical behaviour), so the outcome is always
            // "no error" on a non-error reply.
            const auto parsed = daemonreply::parseWsAuthReply(reply.arguments());
            if (!applyOutcome(QString())) {
                return;
            }
            if (parsed.ok) {
                m_wsAuthMethods = parsed.methods;
                m_oidcIssuer = parsed.oidcIssuer;
                m_oidcAuthEndpoint = parsed.oidcAuthEndpoint;
                m_oidcTokenEndpoint = parsed.oidcTokenEndpoint;
                m_oidcClientId = parsed.oidcClientId;
                m_oidcScopes = parsed.oidcScopes;
                Q_EMIT wsAuthMethodsChanged();
                Q_EMIT oidcIssuerChanged();
                Q_EMIT oidcAuthEndpointChanged();
                Q_EMIT oidcTokenEndpointChanged();
                Q_EMIT oidcClientIdChanged();
                Q_EMIT oidcScopesChanged();
            }
        });

    // Personality (adele-kde#42): the seven traits have no granular getter, so
    // the aggregate GetConfig is the only read path. QtDBus flattens the
    // returned ConfigData struct into one positional arg per field; the
    // personality u32s are the trailing block after CONFIG_DATA_BASE_FIELDS.
    //
    // KDE-8 (#63): the block is read by FIXED positional index, so a daemon field
    // inserted ahead of it would silently shift plausible-looking garbage into
    // the 0..4-clamped sliders. Guard against that by validating the reply
    // against the expected ConfigData signature before indexing
    // (parsePersonalityConfig): the leading fields must match
    // CONFIG_DATA_BASE_SIGNATURE and the reply must end in exactly seven u32
    // traits. On a clean validation we populate and mark the schema OK (so the
    // SetConfig push is allowed). A daemon error (down) keeps defaults and is the
    // section's recorded outcome. A bare pre-#226 daemon (exactly the base
    // fields, no personality block) is the expected older case — keep defaults,
    // no error, schema stays not-ok so we never push. Any other shape is a schema
    // mismatch: keep defaults, leave the schema NOT-ok (blocks the positional
    // push), and surface a clear status (it ranks below an outright daemon error
    // in load()'s status precedence). m_personalitySchemaOk is reset before the
    // reads (see top of load()), so a superseded/never-arriving reply leaves the
    // push correctly disabled.
    asyncSettingsCall(
        QStringLiteral("GetConfig"), {}, DBUS_TIMEOUT_DEFAULT_MS,
        [this, applyOutcome](const QDBusMessage &reply) {
            if (reply.type() == QDBusMessage::ErrorMessage) {
                applyOutcome(dbusErrorMessage(reply));
                return;
            }
            const auto configArgs = reply.arguments();
            if (configArgs.size() == CONFIG_DATA_BASE_FIELDS) {
                // Pre-#226 daemon: no personality surface. Keep defaults; this is
                // not an error, and the schema stays not-ok so we never push.
                applyOutcome(QString());
                return;
            }
            const auto pc = daemonreply::parsePersonalityConfig(
                configArgs,
                QString::fromLatin1(CONFIG_DATA_BASE_SIGNATURE),
                PERSONALITY_TRAIT_COUNT);
            // A schema mismatch is this section's recorded error; a clean parse is
            // a clean outcome. Either way applyOutcome decides staleness + status.
            if (!applyOutcome(pc.signatureOk ? QString() : pc.error)) {
                return; // superseded — drop without touching fields/schema flag
            }
            if (pc.signatureOk) {
                m_personalitySchemaOk = true;
                m_personalityProfessionalism = pc.professionalism;
                m_personalityWarmth = pc.warmth;
                m_personalityDirectness = pc.directness;
                m_personalityEnthusiasm = pc.enthusiasm;
                m_personalityHumor = pc.humor;
                m_personalitySarcasm = pc.sarcasm;
                m_personalityPretentiousness = pc.pretentiousness;
                Q_EMIT personalityChanged();
            }
        });

    // --- Local-only steps: run immediately, never gated on the daemon --------
    loadWidgetConnectionSettings();
    Q_EMIT connectionNamesChanged();
    Q_EMIT defaultConnectionNameChanged();
    emitConnectionSelectionChanged();

    // Probe the voice service + read its config so the Voice tab is populated
    // on open. This emits voiceChanged/voiceConfigChanged itself.
    loadVoiceSettings();

    // Re-probe whenever the voice daemon comes or goes on the session bus.
    // "Restart voice service" / "Apply now" call systemctl restart, which
    // returns as soon as the process is spawned — before the daemon has
    // re-acquired org.desktopAssistant.Voice. The immediate loadVoiceSettings()
    // in restartVoiceService() therefore sees the name unregistered, clears the
    // voice list, and the picker latches disabled with no later re-check. This
    // watcher fires when the name is (re)acquired ~1s later and reloads the
    // live state, so the picker re-enables on its own (and also tracks external
    // start/stop of the daemon while the KCM is open). WatchForOwnerChange
    // covers both registration and unregistration.
    //
    // load() can run more than once for the same KCM instance (System Settings
    // re-entry, an explicit reload). Install the watcher only once; without this
    // guard each load() leaked another watcher, so loadVoiceSettings() fired N
    // times per owner change after N loads (KDE-9).
    if (m_voiceWatcher == nullptr) {
        m_voiceWatcher = new QDBusServiceWatcher(
            QString::fromUtf8(VOICE_SERVICE),
            QDBusConnection::sessionBus(),
            QDBusServiceWatcher::WatchForOwnerChange,
            this);
        connect(m_voiceWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString &, const QString &, const QString &) {
                    loadVoiceSettings();
                });
    }

    setNeedsSave(false);
}

void DesktopAssistantKcm::save()
{
    // Immediate-save throughout: each setter has already pushed its
    // change via the corresponding D-Bus method, so save() is a no-op.
    // Kept for KQuickConfigModule's vtable / for any future hooks.
}

void DesktopAssistantKcm::pushSetterAsync(const QString &method, const QVariantList &args)
{
    // Fire-and-forget setter push (KDE-2 / #57, PR 3/5). Every per-section
    // setter used to do a synchronous QDBusInterface::call — which both blocked
    // on the constructor's introspection round-trip AND on the reply — so a
    // wedged daemon froze System Settings on every keystroke/toggle. We now hand
    // off to asyncSettingsCall and only consult the reply to surface an error
    // into statusText; nothing waits on the UI thread. The watcher is parented
    // to `this`, so a reply that lands after the KCM is gone is simply dropped.
    asyncSettingsCall(
        method, args, DBUS_TIMEOUT_DEFAULT_MS,
        [this](const QDBusMessage &reply) {
            // Only an error needs surfacing; success is silent (the setter has
            // already updated the in-memory value + emitted its NOTIFY).
            setStatusFromDbusError(reply);
        });
}

void DesktopAssistantKcm::pushPersistenceSettings()
{
    pushSetterAsync(QStringLiteral("SetPersistenceSettings"),
                    {m_gitEnabled, m_gitRemoteUrl, m_gitRemoteName, m_gitPushOnUpdate});
}

void DesktopAssistantKcm::pushDatabaseSettings()
{
    pushSetterAsync(QStringLiteral("SetDatabaseSettings"),
                    {m_dbUrl, static_cast<uint>(m_dbMaxConnections)});
}

void DesktopAssistantKcm::pushBackendTasksSettings()
{
    pushSetterAsync(QStringLiteral("SetBackendTasksSettings"),
                    {m_btLlmConnector,
                     m_btLlmModel,
                     m_btLlmBaseUrl,
                     m_btDreamingEnabled,
                     static_cast<qulonglong>(m_btDreamingIntervalSecs),
                     static_cast<uint>(m_btArchiveAfterDays)});
}

void DesktopAssistantKcm::pushWsAuthSettings()
{
    pushSetterAsync(QStringLiteral("SetWsAuthSettings"),
                    {m_wsAuthMethods,
                     m_oidcIssuer,
                     m_oidcAuthEndpoint,
                     m_oidcTokenEndpoint,
                     m_oidcClientId,
                     m_oidcScopes});
}

void DesktopAssistantKcm::defaults()
{
    // The legacy "apply connector defaults" path drove the removed single-LLM /
    // embeddings / backend-LLM fields (desktop-assistant#17). Model selection
    // now lives on the Connections + Purposes pages, which own their own
    // defaults; there is nothing for this KCM-level reset to do.
    m_statusText = QStringLiteral("No default settings to apply");
    Q_EMIT statusTextChanged();
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

QString DesktopAssistantKcm::dbusErrorMessage(const QDBusMessage &message)
{
    QString text = message.errorMessage();
    if (text.isEmpty()) {
        text = QStringLiteral("D-Bus call failed");
    }
    return text;
}

bool DesktopAssistantKcm::setStatusFromDbusError(const QDBusMessage &message)
{
    if (message.type() != QDBusMessage::ErrorMessage) {
        return false;
    }

    m_statusText = dbusErrorMessage(message);
    Q_EMIT statusTextChanged();
    return true;
}

void DesktopAssistantKcm::asyncSettingsCall(const QString &method,
                                            const QVariantList &args,
                                            int timeoutMs,
                                            std::function<void(const QDBusMessage &)> handler,
                                            const char *service,
                                            const char *path,
                                            const char *iface)
{
    // Build the call by hand: QDBusInterface's constructor performs a blocking
    // introspection round-trip (the very stall KDE-2 / #57 removes), so we use
    // createMethodCall + asyncCall instead — no introspection, no UI-thread wait.
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromUtf8(service ? service : SERVICE),
        QString::fromUtf8(path ? path : PATH),
        QString::fromUtf8(iface ? iface : IFACE),
        method);
    if (!args.isEmpty()) {
        msg.setArguments(args);
    }

    QDBusPendingCall pending =
        QDBusConnection::sessionBus().asyncCall(msg, timeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [handler = std::move(handler)](QDBusPendingCallWatcher *w) {
                handler(w->reply());
                w->deleteLater();
            });
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

    const QFileInfo fileInfo(widgetSettingsPath());
    QDir dir;
    if (!dir.mkpath(fileInfo.absolutePath())) {
        m_statusText = QStringLiteral("Unable to create widget settings directory");
        Q_EMIT statusTextChanged();
        return false;
    }

    // KDE-7 (#62): atomic write via QSaveFile (temp file + rename on commit), so
    // a crash mid-write never leaves a truncated widget_settings.json. Matches
    // the voice config path; a failed write leaves the old file untouched.
    QSaveFile file(widgetSettingsPath());
    if (!file.open(QIODevice::WriteOnly)) {
        m_statusText = QStringLiteral("Unable to write widget settings file");
        Q_EMIT statusTextChanged();
        return false;
    }

    const QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        m_statusText = QStringLiteral("Unable to write widget settings file");
        Q_EMIT statusTextChanged();
        return false;
    }
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

// Normalize a `wake_word.listening_cue` value to the daemon's enum tokens. The
// KCM now offers a fixed picker (ding/phrase/off), but a hand-edited or legacy
// config may hold anything; map a stray value to the default (empty -> omitted
// on write -> ding) rather than writing it back verbatim and breaking the
// daemon's config parse.
QString normalizeListeningCue(const QString &value)
{
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("ding") || v == QLatin1String("phrase")
        || v == QLatin1String("off")) {
        return v;
    }
    return QString();
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
    // Ask the bus whether the name currently has an owner. We must go through
    // QDBusConnectionInterface (isServiceRegistered) rather than constructing a
    // raw QDBusInterface to org.freedesktop.DBus: the latter comes back
    // isValid() == false (the bus daemon object isn't usable as a generic
    // remote interface), which made this function ALWAYS return false and left
    // the voice picker permanently disabled. isServiceRegistered checks current
    // ownership only — it does NOT D-Bus-activate the service, so a masked /
    // uninstalled daemon still reports false instead of being spawned.
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (bus == nullptr) {
        return false;
    }
    QDBusReply<bool> reply = bus->isServiceRegistered(QString::fromUtf8(VOICE_SERVICE));
    return reply.isValid() && reply.value();
}

void DesktopAssistantKcm::runSystemctlUserAsync(
    const QStringList &args,
    std::function<void(const QString &out, bool ok)> done)
{
    // Non-blocking `systemctl --user <args>` (KDE-2 / #57, PR 5/5). The old
    // synchronous QProcess waitForStarted/waitForFinished blocked the System
    // Settings UI thread up to ~8s (e.g. on a slow/hung systemd). We now spawn
    // the process parented to `this` and report its trimmed stdout + ok (exit==0)
    // through `done`, fired exactly once from finished/errorOccurred on the UI
    // thread. The process self-deletes via deleteLater; if the KCM is destroyed
    // first the parent-child teardown drops the pending callback.
    auto *proc = new QProcess(this);
    auto fired = std::make_shared<bool>(false);
    auto finish = [proc, fired, done = std::move(done)](const QString &out, bool ok) {
        if (*fired) {
            return; // finished + errorOccurred can both arrive; report once.
        }
        *fired = true;
        done(out, ok);
        proc->deleteLater();
    };

    connect(proc, &QProcess::errorOccurred, this,
            [finish](QProcess::ProcessError) { finish(QString(), false); });
    connect(proc, &QProcess::finished, this,
            [proc, finish](int exitCode, QProcess::ExitStatus status) {
                const QString out =
                    QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
                finish(out, status == QProcess::NormalExit && exitCode == 0);
            });

    proc->start(QStringLiteral("systemctl"), QStringList{QStringLiteral("--user")} + args);
}

void DesktopAssistantKcm::probeVoiceAutostartAsync()
{
    // `systemctl --user is-enabled adele-voice.service` (now async, KDE-2 / #57
    // PR 5/5). Maps the reported state to the tri-state m_voiceAutostart via the
    // bus-free daemonreply::autostartStateToTriState helper, then emits
    // voiceChanged so the toggle reflects reality. Re-issued whenever the unit
    // state may have changed (load, enable/disable, restart).
    runSystemctlUserAsync(
        QStringList{QStringLiteral("is-enabled"), QString::fromUtf8(VOICE_UNIT)},
        [this](const QString &state, bool /*ok*/) {
            const int tri = daemonreply::autostartStateToTriState(state);
            if (m_voiceAutostart != tri) {
                m_voiceAutostart = tri;
                Q_EMIT voiceChanged();
            }
        });
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
    m_kokoroVoice = QStringLiteral("af_heart");
    m_pollyVoice = QStringLiteral("Joanna");

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
        // KDE-6 (#61): strip a trailing inline comment (` #...`, honouring
        // quotes) BEFORE parsing the scalar — otherwise `sensitivity = 0.45 #x`
        // failed toDouble() and the UI silently reverted to a default the user
        // never chose, and the next write then deleted the comment.
        QString value = voiceconfig::stripInlineComment(trimmed.mid(eq + 1));
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
                m_listeningCue = normalizeListeningCue(value);
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
            } else if (key == QLatin1String("kokoro_voice")) {
                m_kokoroVoice = value;
            } else if (key == QLatin1String("polly_voice")) {
                m_pollyVoice = value;
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

    // The owned keys, marshalled into voiceconfig::Target. The merge (including
    // KDE-6 inline-comment preservation) is a pure, unit-tested helper
    // (voiceconfig::mergeTomlLines) so the file I/O below stays thin.
    using voiceconfig::Target;
    QVector<Target> targets = {
        {QStringLiteral("audio"), QStringLiteral("input_device"), m_inputDevice, true, false},
        {QStringLiteral("audio"), QStringLiteral("output_device"), m_outputDevice, true, false},
        {QStringLiteral("wake_word"), QStringLiteral("sensitivity"),
         formatTomlFloat(m_wakeSensitivity), false, false},
        // Forward-compat wake-word keys (voice#50/#51). `eager` is a bare bool;
        // `listening_cue` is omit-when-empty so an unset cue doesn't pin a value.
        {QStringLiteral("wake_word"), QStringLiteral("eager"),
         m_wakeEager ? QStringLiteral("true") : QStringLiteral("false"), false, false},
        {QStringLiteral("wake_word"), QStringLiteral("listening_cue"), m_listeningCue, true, true},
        // Endpointing (adele-kde#37): [vad] + [assistant].
        {QStringLiteral("vad"), QStringLiteral("speech_threshold"),
         formatTomlFloat(m_vadSpeechThreshold), false, false},
        {QStringLiteral("vad"), QStringLiteral("silence_duration_ms"),
         QString::number(m_vadSilenceDurationMs), false, false},
        {QStringLiteral("assistant"), QStringLiteral("followup_timeout_ms"),
         QString::number(m_followupTimeoutMs), false, false},
        {QStringLiteral("stt"), QStringLiteral("language"), m_sttLanguage, true, false},
        {QStringLiteral("stt"), QStringLiteral("model_path"), m_sttModelPath, true, true},
        {QStringLiteral("tts"), QStringLiteral("backend"), m_ttsBackend, true, false},
        {QStringLiteral("tts"), QStringLiteral("kokoro_lang"), m_kokoroLang, true, false},
        // Persisted voice selection so a restart keeps the user's pick instead
        // of falling back to the daemon default (see setVoice). omit-when-empty
        // so clearing it restores the daemon default.
        {QStringLiteral("tts"), QStringLiteral("kokoro_voice"), m_kokoroVoice, true, true},
        {QStringLiteral("tts"), QStringLiteral("polly_voice"), m_pollyVoice, true, true},
        {QStringLiteral("tts"), QStringLiteral("model_path"), m_piperModelPath, true, true},
        {QStringLiteral("tts"), QStringLiteral("polly_engine"), m_pollyEngine, true, false},
        {QStringLiteral("tts"), QStringLiteral("polly_region"), m_pollyRegion, true, true},
    };

    const QStringList merged = voiceconfig::mergeTomlLines(lines, targets);

    QFileInfo fileInfo(path);
    QDir dir;
    if (!dir.mkpath(fileInfo.absolutePath())) {
        m_statusText = QStringLiteral("Unable to create voice config directory");
        Q_EMIT statusTextChanged();
        return false;
    }
    // KDE-7 (#62): atomic write. QSaveFile writes to a temp file and renames it
    // into place on commit(), so a crash / power-loss mid-write can never leave
    // the voice daemon a truncated config.toml it can't parse (the crash-loop
    // class the daemon side just fixed). A failed write leaves the old file
    // untouched.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_statusText = QStringLiteral("Unable to write voice config file");
        Q_EMIT statusTextChanged();
        return false;
    }
    QTextStream stream(&out);
    for (const QString &l : merged) {
        stream << l << '\n';
    }
    stream.flush();
    if (!out.commit()) {
        m_statusText = QStringLiteral("Unable to write voice config file");
        Q_EMIT statusTextChanged();
        return false;
    }
    return true;
}

void DesktopAssistantKcm::scheduleVoiceConfigWrite()
{
    // KDE-7 (#62): coalesce a burst of setter calls (a slider drag fires its
    // setter per tick) into a single config.toml write once the value settles.
    // Re-arming the single-shot timer on each call collapses the burst; the
    // delay is short enough to feel immediate to the user but long enough to
    // outlast inter-tick gaps during a drag. flushVoiceConfigWrite() commits any
    // pending write synchronously when a later action depends on it.
    constexpr int kVoiceWriteDebounceMs = 400;
    if (m_voiceWriteDebounce == nullptr) {
        m_voiceWriteDebounce = new QTimer(this);
        m_voiceWriteDebounce->setSingleShot(true);
        connect(m_voiceWriteDebounce, &QTimer::timeout, this, [this]() {
            writeVoiceConfig();
        });
    }
    m_voiceWriteDebounce->start(kVoiceWriteDebounceMs);
}

bool DesktopAssistantKcm::flushVoiceConfigWrite()
{
    // Cancel any pending debounced write and persist immediately. Used by the
    // paths that read or act on config.toml right after changing it (reset
    // buttons, Apply/restart, SetVoice), so a debounced change is never lost or
    // applied a beat late. Always writes (even with no timer armed) so callers
    // get a single, authoritative on-disk state.
    if (m_voiceWriteDebounce != nullptr) {
        m_voiceWriteDebounce->stop();
    }
    return writeVoiceConfig();
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
        // No live daemon to toggle: this is a pure no-op — m_voiceEnabled is
        // left unchanged (we don't flip it speculatively), and the checkbox
        // re-asserts itself from kcm.voiceEnabled on the next voiceChanged /
        // loadVoiceSettings() once the daemon reappears (see KDE-10).
        return;
    }
    // Async SetEnabled (KDE-2 / #57, PR 4/5 — was a blocking
    // QDBusInterface::call against the voice interface). We only flip
    // m_voiceEnabled + status once the reply lands, so a wedged voice daemon
    // can't stall the toggle on the UI thread. The watcher is parented to `this`.
    asyncSettingsCall(
        QStringLiteral("SetEnabled"), {value}, DBUS_TIMEOUT_DEFAULT_MS,
        [this, value](const QDBusMessage &reply) {
            if (reply.type() == QDBusMessage::ErrorMessage) {
                m_statusText = reply.errorMessage().isEmpty()
                    ? QStringLiteral("Failed to toggle voice")
                    : reply.errorMessage();
                Q_EMIT statusTextChanged();
                return;
            }
            m_voiceEnabled = value;
            m_statusText = value ? QStringLiteral("“Hey Adele” enabled")
                                 : QStringLiteral("“Hey Adele” disabled");
            Q_EMIT voiceChanged();
            Q_EMIT statusTextChanged();
        },
        VOICE_SERVICE, VOICE_PATH, VOICE_IFACE);
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
}

// --- Whisper STT model selector (adele-kde#44) ------------------------------

QString DesktopAssistantKcm::sttModelsDir() const
{
    // Match the voice daemon's resolution: $XDG_DATA_HOME/adele-voice/models
    // (GenericDataLocation honours XDG_DATA_HOME and falls back to
    // ~/.local/share). The path is absolute, which is what we write into
    // stt.model_path (the daemon does not tilde-expand).
    const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(dataHome).filePath(QStringLiteral("adele-voice/models"));
}

bool DesktopAssistantKcm::sttModelInstalled(const QString &fileOrPath) const
{
    const QString trimmed = fileOrPath.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    // A bare basename resolves against the models dir; anything that looks like a
    // path (absolute or containing a separator) is checked as given.
    const bool looksLikePath = trimmed.contains(QLatin1Char('/'));
    const QString resolved =
        looksLikePath ? trimmed : QDir(sttModelsDir()).filePath(trimmed);
    return QFileInfo::exists(resolved);
}

bool DesktopAssistantKcm::sttDownloadActive() const
{
    return m_sttDownloadActive;
}

int DesktopAssistantKcm::sttDownloadProgress() const
{
    return m_sttDownloadProgress;
}

QString DesktopAssistantKcm::sttDownloadError() const
{
    return m_sttDownloadError;
}

QString DesktopAssistantKcm::sttDownloadingFile() const
{
    return m_sttDownloadingFile;
}

void DesktopAssistantKcm::resetSttDownloadState()
{
    m_sttDownloadActive = false;
    m_sttDownloadProgress = -1;
    m_sttDownloadingFile.clear();
    m_sttDownloadTempPath.clear();
    m_sttDownloadDestPath.clear();
    Q_EMIT sttDownloadChanged();
}

void DesktopAssistantKcm::cleanupSttDownload(bool keepError)
{
    if (m_sttReply != nullptr) {
        m_sttReply->disconnect(this);
        if (m_sttReply->isRunning()) {
            m_sttReply->abort();
        }
        m_sttReply->deleteLater();
        m_sttReply = nullptr;
    }
    if (m_sttTempFile != nullptr) {
        if (m_sttTempFile->isOpen()) {
            m_sttTempFile->close();
        }
        if (!m_sttDownloadTempPath.isEmpty()) {
            QFile::remove(m_sttDownloadTempPath);
        }
        delete m_sttTempFile;
        m_sttTempFile = nullptr;
    }
    if (!keepError) {
        m_sttDownloadError.clear();
    }
    resetSttDownloadState();
}

void DesktopAssistantKcm::cancelSttModelDownload()
{
    if (!m_sttDownloadActive) {
        return;
    }
    cleanupSttDownload(/*keepError=*/false);
    m_statusText = QStringLiteral("Model download cancelled");
    Q_EMIT statusTextChanged();
}

void DesktopAssistantKcm::downloadSttModel(const QString &fileName, const QString &url)
{
    // One download at a time — ignore re-entrant requests rather than racing two
    // transfers onto the same temp/dest path.
    if (m_sttDownloadActive) {
        return;
    }

    const QString cleanName = QFileInfo(fileName.trimmed()).fileName();
    const QUrl src(url.trimmed());
    // Guard against a malformed catalog entry or a non-HTTPS URL — we only fetch
    // model files over https from the curated catalog.
    if (cleanName.isEmpty() || !src.isValid() || src.scheme() != QLatin1String("https")) {
        m_sttDownloadError = QStringLiteral("Invalid model download request");
        m_statusText = m_sttDownloadError;
        Q_EMIT statusTextChanged();
        Q_EMIT sttDownloadChanged();
        return;
    }

    const QString dir = sttModelsDir();
    if (!QDir().mkpath(dir)) {
        m_sttDownloadError = QStringLiteral("Unable to create the models directory");
        m_statusText = m_sttDownloadError;
        Q_EMIT statusTextChanged();
        Q_EMIT sttDownloadChanged();
        return;
    }

    m_sttDownloadDestPath = QDir(dir).filePath(cleanName);
    // Download to a sibling temp file, then atomically rename on success so a
    // half-finished file can never be mistaken for an installed model.
    m_sttDownloadTempPath = m_sttDownloadDestPath + QStringLiteral(".part");
    QFile::remove(m_sttDownloadTempPath);

    m_sttTempFile = new QFile(m_sttDownloadTempPath);
    if (!m_sttTempFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete m_sttTempFile;
        m_sttTempFile = nullptr;
        m_sttDownloadError = QStringLiteral("Unable to open a temporary file for download");
        m_statusText = m_sttDownloadError;
        Q_EMIT statusTextChanged();
        Q_EMIT sttDownloadChanged();
        return;
    }

    if (m_sttNam == nullptr) {
        m_sttNam = new QNetworkAccessManager(this);
    }

    m_sttDownloadActive = true;
    m_sttDownloadProgress = 0;
    m_sttDownloadingFile = cleanName;
    m_sttDownloadError.clear();
    m_statusText = QStringLiteral("Downloading %1…").arg(cleanName);
    Q_EMIT statusTextChanged();
    Q_EMIT sttDownloadChanged();

    QNetworkRequest request(src);
    // HuggingFace `resolve/main` URLs 302-redirect to a CDN; allow it.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("adele-kde-kcm/1.0"));

    m_sttReply = m_sttNam->get(request);

    connect(m_sttReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_sttTempFile != nullptr && m_sttReply != nullptr) {
            m_sttTempFile->write(m_sttReply->readAll());
        }
    });
    connect(m_sttReply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                const int pct = (total > 0)
                    ? static_cast<int>((received * 100) / total)
                    : -1;
                if (pct != m_sttDownloadProgress) {
                    m_sttDownloadProgress = pct;
                    Q_EMIT sttDownloadChanged();
                }
            });
    connect(m_sttReply, &QNetworkReply::finished, this, [this, cleanName]() {
        if (m_sttReply == nullptr) {
            return;
        }
        const bool ok = m_sttReply->error() == QNetworkReply::NoError;
        const QString errString = m_sttReply->errorString();

        // Flush any trailing buffered bytes before we evaluate success.
        if (m_sttTempFile != nullptr) {
            if (m_sttReply->bytesAvailable() > 0) {
                m_sttTempFile->write(m_sttReply->readAll());
            }
            m_sttTempFile->flush();
            m_sttTempFile->close();
        }

        m_sttReply->deleteLater();
        m_sttReply = nullptr;

        if (!ok) {
            // Failure: drop the temp file, surface the error, clear in-flight.
            if (m_sttTempFile != nullptr) {
                delete m_sttTempFile;
                m_sttTempFile = nullptr;
            }
            QFile::remove(m_sttDownloadTempPath);
            m_sttDownloadError = errString.isEmpty()
                ? QStringLiteral("Download failed")
                : errString;
            m_statusText = QStringLiteral("Download of %1 failed: %2")
                               .arg(cleanName, m_sttDownloadError);
            Q_EMIT statusTextChanged();
            // keepError so the QML warning row can show what went wrong.
            cleanupSttDownload(/*keepError=*/true);
            return;
        }

        delete m_sttTempFile;
        m_sttTempFile = nullptr;

        // Atomic publish: replace any stale dest, then rename temp -> dest.
        QFile::remove(m_sttDownloadDestPath);
        const bool renamed = QFile::rename(m_sttDownloadTempPath, m_sttDownloadDestPath);
        if (!renamed) {
            QFile::remove(m_sttDownloadTempPath);
            m_sttDownloadError = QStringLiteral("Could not move the downloaded model into place");
            m_statusText = m_sttDownloadError;
            Q_EMIT statusTextChanged();
            cleanupSttDownload(/*keepError=*/true);
            return;
        }

        m_statusText = QStringLiteral("Downloaded %1").arg(cleanName);
        Q_EMIT statusTextChanged();
        // Clear in-flight state (no error), then nudge the page to re-evaluate
        // presence so the "(not downloaded)" annotation/warning clears.
        cleanupSttDownload(/*keepError=*/false);
        Q_EMIT voiceConfigChanged();
    });
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
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
    scheduleVoiceConfigWrite();
}

QString DesktopAssistantKcm::listeningCue() const
{
    return m_listeningCue;
}

void DesktopAssistantKcm::setListeningCue(const QString &value)
{
    const QString normalized = normalizeListeningCue(value);
    if (m_listeningCue == normalized) {
        return;
    }
    m_listeningCue = normalized;
    Q_EMIT voiceConfigChanged();
    scheduleVoiceConfigWrite();
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
    // Bump the voice-load generation (KDE-2 / #57, PR 4/5) so any async voice
    // read issued by a PREVIOUS loadVoiceSettings() that finishes after THIS one
    // started detects it was superseded and drops its reply. loadVoiceSettings()
    // is re-fired often (every load(), the service watcher on owner-change, and
    // restart/apply), so without this a slow stale reply from a daemon that just
    // went away could clobber the fresh "unavailable" state.
    const quint64 generation = ++m_voiceLoadGeneration;

    m_voiceServiceAvailable = probeVoiceAvailable();

    if (!m_voiceServiceAvailable) {
        m_voiceEnabled = false;
        m_voiceList.clear();
        m_voiceCurrentId.clear();
        m_voiceCurrentSpeaker = -1;
    }

    // Local-only steps run IMMEDIATELY, never gated on the voice daemon, so the
    // page (TOML-backed config fields) is populated even when the daemon is
    // wedged or absent. readVoiceConfig() is pure file I/O; the autostart probe
    // is now an async systemctl subprocess (KDE-2 / #57, PR 5/5) that updates
    // m_voiceAutostart + emits voiceChanged when it lands.
    // NB: device enumeration (pactl + arecord/aplay subprocesses) is NOT done
    // here — loadVoiceSettings() runs from the KCM constructor's load(), which
    // fires for every tab of this settings module, so spawning audio tools then
    // would add startup latency even for users who never open the Voice tab. The
    // Voice page calls loadAudioDevices() itself from Component.onCompleted.
    probeVoiceAutostartAsync();
    readVoiceConfig();
    Q_EMIT voiceChanged();
    Q_EMIT voiceConfigChanged();

    if (!m_voiceServiceAvailable) {
        // Nothing live to read; the cleared state above is final for this pass.
        return;
    }

    // --- Async live-state reads (KDE-2 / #57, PR 4/5) ------------------------
    // GetEnabled / ListVoices / GetVoice were three serial blocking
    // QDBusInterface::call round-trips on the UI thread. Fire each on its own
    // watcher against the voice interface; each handler drops its reply when a
    // newer loadVoiceSettings() has started (generation guard) and otherwise
    // updates only its own field(s) and emits voiceChanged. The watchers are
    // parented to `this`, so a reply landing after the KCM is gone is dropped.

    // GetEnabled -> b.
    asyncSettingsCall(
        QStringLiteral("GetEnabled"), {}, DBUS_TIMEOUT_DEFAULT_MS,
        [this, generation](const QDBusMessage &reply) {
            if (generation != m_voiceLoadGeneration) {
                return; // superseded
            }
            if (reply.type() == QDBusMessage::ErrorMessage) {
                return; // keep current; don't surface (load is best-effort)
            }
            const auto args = reply.arguments();
            if (!args.isEmpty()) {
                m_voiceEnabled = args.first().toBool();
                Q_EMIT voiceChanged();
            }
        },
        VOICE_SERVICE, VOICE_PATH, VOICE_IFACE);

    // ListVoices -> a(sssu). Marshal into a QVariantList of maps the QML page
    // reads by key (voice_id / display_name / language / num_speakers). This
    // demarshalling needs a live QDBusArgument, so it stays inline (not a
    // bus-free helper).
    asyncSettingsCall(
        QStringLiteral("ListVoices"), {}, DBUS_TIMEOUT_DEFAULT_MS,
        [this, generation](const QDBusMessage &reply) {
            if (generation != m_voiceLoadGeneration) {
                return; // superseded
            }
            if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
                return;
            }
            QVariantList voices;
            const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
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
                voices.push_back(entry);
            }
            arg.endArray();
            m_voiceList = voices;
            Q_EMIT voiceChanged();
        },
        VOICE_SERVICE, VOICE_PATH, VOICE_IFACE);

    // GetVoice -> (si): (voice_id, speaker_id); speaker_id -1 if unset. The flat
    // form is parsed by the bus-free helper; the wrapped single-QDBusArgument
    // form still needs live demarshalling inline.
    asyncSettingsCall(
        QStringLiteral("GetVoice"), {}, DBUS_TIMEOUT_DEFAULT_MS,
        [this, generation](const QDBusMessage &reply) {
            if (generation != m_voiceLoadGeneration) {
                return; // superseded
            }
            if (reply.type() == QDBusMessage::ErrorMessage) {
                return;
            }
            const auto args = reply.arguments();
            const auto sel = daemonreply::parseVoiceSelectionReply(args);
            if (sel.ok) {
                m_voiceCurrentId = sel.voiceId;
                m_voiceCurrentSpeaker = sel.speaker;
                Q_EMIT voiceChanged();
            } else if (args.size() == 1) {
                // Some bindings wrap the struct; unpack via QDBusArgument.
                const QDBusArgument inner = args.first().value<QDBusArgument>();
                inner.beginStructure();
                inner >> m_voiceCurrentId >> m_voiceCurrentSpeaker;
                inner.endStructure();
                Q_EMIT voiceChanged();
            }
        },
        VOICE_SERVICE, VOICE_PATH, VOICE_IFACE);
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
    // Async SetVoice (KDE-2 / #57, PR 4/5 — was a blocking QDBusInterface::call
    // against the voice interface). The config persist + status only happen once
    // the reply confirms success, so a wedged voice daemon can't stall the
    // picker on the UI thread. The watcher is parented to `this`.
    asyncSettingsCall(
        QStringLiteral("SetVoice"), {id, speaker}, DBUS_TIMEOUT_DEFAULT_MS,
        [this, id, speaker](const QDBusMessage &reply) {
            if (reply.type() == QDBusMessage::ErrorMessage) {
                m_statusText = reply.errorMessage().isEmpty()
                    ? QStringLiteral("Failed to set voice")
                    : reply.errorMessage();
                Q_EMIT statusTextChanged();
                return;
            }
            m_voiceCurrentId = id;
            m_voiceCurrentSpeaker = speaker;

            // SetVoice above only changes the RUNNING daemon. Persist the choice
            // to config.toml under the active backend's voice key as well, so a
            // restart (or "Restart voice service") reloads it instead of falling
            // back to the daemon default. The key is backend-specific (repo
            // adelie-ai/voice, crates/module/src/config.rs): Kokoro ->
            // kokoro_voice, Polly -> polly_voice, Piper -> model_path (the daemon
            // resolves <models_dir>/<id>.onnx, and our sttModelsDir() matches
            // that models dir).
            if (m_ttsBackend == QLatin1String("polly")) {
                m_pollyVoice = id;
            } else if (m_ttsBackend == QLatin1String("piper")) {
                m_piperModelPath = QDir(sttModelsDir()).filePath(id + QStringLiteral(".onnx"));
            } else {
                m_kokoroVoice = id;
            }
            flushVoiceConfigWrite();

            m_statusText = QStringLiteral("Voice set to %1").arg(id);
            Q_EMIT voiceChanged();
            Q_EMIT voiceConfigChanged();
            Q_EMIT statusTextChanged();
        },
        VOICE_SERVICE, VOICE_PATH, VOICE_IFACE);
}

void DesktopAssistantKcm::setVoiceAutostart(bool enabled)
{
    if (m_voiceAutostart < 0) {
        // Unit isn't installed — nothing to enable/disable.
        m_statusText = QStringLiteral("Voice service unit is not installed");
        Q_EMIT statusTextChanged();
        return;
    }
    // Async enable/disable (KDE-2 / #57, PR 5/5 — was a blocking systemctl
    // subprocess). Surface a provisional "working" status now, then the real
    // outcome + a fresh autostart re-probe when the process finishes.
    m_statusText = enabled ? QStringLiteral("Enabling voice autostart…")
                           : QStringLiteral("Disabling voice autostart…");
    Q_EMIT statusTextChanged();
    runSystemctlUserAsync(
        QStringList{enabled ? QStringLiteral("enable") : QStringLiteral("disable"),
                    QString::fromUtf8(VOICE_UNIT)},
        [this, enabled](const QString & /*out*/, bool ok) {
            if (!ok) {
                m_statusText = enabled
                    ? QStringLiteral("Failed to enable voice autostart")
                    : QStringLiteral("Failed to disable voice autostart");
            } else {
                m_statusText = enabled
                    ? QStringLiteral("Voice will start at login")
                    : QStringLiteral("Voice autostart disabled");
            }
            Q_EMIT statusTextChanged();
            // Re-probe so the toggle reflects the real unit state (enable can be
            // refused, e.g. for a static unit); this emits voiceChanged itself.
            probeVoiceAutostartAsync();
        });
}

void DesktopAssistantKcm::restartVoiceService()
{
    // Config-file settings (TTS backend + per-backend keys, STT, devices,
    // sensitivity) only take effect on (re)start, so this applies them without
    // leaving the page. `restart` starts the unit if it was stopped, which is
    // the behaviour we want from a "Restart voice service" button.
    //
    // Flush any debounced config write (KDE-7 / #62) first so the daemon re-reads
    // the user's latest values, not a stale file from before an in-flight write.
    flushVoiceConfigWrite();
    // Async restart (KDE-2 / #57, PR 5/5 — was a blocking systemctl subprocess).
    // Show a provisional status now, then the outcome + a reconcile when it
    // finishes.
    m_statusText = QStringLiteral("Restarting voice service…");
    Q_EMIT statusTextChanged();
    runSystemctlUserAsync(
        QStringList{QStringLiteral("restart"), QString::fromUtf8(VOICE_UNIT)},
        [this](const QString & /*out*/, bool ok) {
            m_statusText = ok ? QStringLiteral("Voice service restarted")
                              : QStringLiteral("Failed to restart the voice service");
            Q_EMIT statusTextChanged();
            // The restart re-spawns the daemon and re-reads config; reconcile the
            // page (availability, enabled, voice list, autostart) against the new
            // process. systemctl returns before the daemon has re-acquired its bus
            // name, so the live reads here may still see it absent — the
            // QDBusServiceWatcher installed in load() re-fires loadVoiceSettings()
            // once the name reappears, which re-enables the picker on its own.
            loadVoiceSettings();
        });
}

void DesktopAssistantKcm::tryDaemonReload(std::function<void(bool)> done)
{
    // Forward-compatible hot-reload (adele-kde#37). The daemon exposes a
    // `Reload` method on org.desktopAssistant.Voice (voice#52) that re-reads
    // config.toml without a restart. Until that lands the call comes back as
    // UnknownMethod, which we treat as "not supported" -> done(false) so the
    // caller falls back to a service restart. We only attempt this when the
    // service is actually on the bus (an async call to an absent name would
    // D-Bus *activate* the daemon, which we don't want from a probe).
    //
    // KDE-2 / #57, PR 4/5: was a blocking QDBusInterface::call. Now async — the
    // result is delivered through `done` on the UI thread. The watcher is
    // parented to `this`.
    if (!m_voiceServiceAvailable) {
        done(false);
        return;
    }
    asyncSettingsCall(
        QStringLiteral("Reload"), {}, DBUS_TIMEOUT_DEFAULT_MS,
        [done = std::move(done)](const QDBusMessage &reply) {
            done(reply.type() != QDBusMessage::ErrorMessage);
        },
        VOICE_SERVICE, VOICE_PATH, VOICE_IFACE);
}

void DesktopAssistantKcm::applyVoiceChanges()
{
    // Persist anything still pending, then apply live. Setters write via a
    // debounce (KDE-7 / #62), so flush any in-flight write synchronously here —
    // this guarantees the on-disk config is current before we ask the daemon to
    // reload/restart, even if Apply was pressed mid-drag.
    flushVoiceConfigWrite();

    // Snapshot the values the fallback branch needs now: m_voiceServiceAvailable
    // / m_voiceAutostart could change (e.g. via the service watcher) before the
    // async Reload reply lands, but the decision below reflects the state at the
    // moment Apply was pressed.
    const bool serviceAvailable = m_voiceServiceAvailable;
    const int autostart = m_voiceAutostart;

    tryDaemonReload([this, serviceAvailable, autostart](bool reloaded) {
        if (reloaded) {
            m_statusText = QStringLiteral("Voice settings reloaded");
            Q_EMIT statusTextChanged();
            // A reload doesn't change availability/voice list, but re-read config
            // so the page reflects exactly what's now on disk.
            loadVoiceSettings();
            return;
        }

        // Fall back to a restart. restartVoiceService() sets its own status and
        // re-reads live state. If the unit isn't installed there's nothing to do
        // beyond the on-disk write we already performed; say so honestly.
        if (autostart < 0 && !serviceAvailable) {
            m_statusText = QStringLiteral(
                "Saved. The voice service isn't running; changes apply when it next starts.");
            Q_EMIT statusTextChanged();
            return;
        }
        restartVoiceService();
    });
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

QVariantList DesktopAssistantKcm::enumerateVoiceInputDevices(bool *ok) const
{
    if (ok) {
        *ok = false;
    }
    const QString bin = QStandardPaths::findExecutable(QStringLiteral("adele-voice"));
    if (bin.isEmpty()) {
        return {};
    }

    QProcess voice;
    voice.start(bin, QStringList{QStringLiteral("list-devices")});
    if (!voice.waitForStarted(2000) || !voice.waitForFinished(5000)
        || voice.exitStatus() != QProcess::NormalExit || voice.exitCode() != 0) {
        return {};
    }

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(voice.readAllStandardOutput(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        return {};
    }

    // Parsed cleanly — the daemon's list is now authoritative, even if empty.
    if (ok) {
        *ok = true;
    }
    QVariantList out;
    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();
        const QString value = obj.value(QStringLiteral("value")).toString();
        // The caller prepends the "default" sentinel itself; skip the daemon's.
        if (value.isEmpty() || value == QLatin1String("default")) {
            continue;
        }
        // Only offer devices capture can actually open. A device that's present
        // but currently in use is still supported (it's the mic you're using).
        if (!obj.value(QStringLiteral("supported")).toBool()) {
            continue;
        }
        QString label = obj.value(QStringLiteral("label")).toString();
        if (label.isEmpty()) {
            label = value;
        }
        // The daemon tags each entry's nature and lists shared routes first.
        // Shared sound-server routes are the recommended choice; raw cards take
        // the mic exclusively and can block other apps (and another logged-in
        // user's session), so flag them.
        const QString kind = obj.value(QStringLiteral("kind")).toString();
        const QString reason = obj.value(QStringLiteral("reason")).toString();
        if (kind == QLatin1String("server")) {
            // Friendlier "follows the sound server" labels for the routes.
            if (label.contains(QLatin1String("PipeWire"), Qt::CaseInsensitive)) {
                label = QStringLiteral("PipeWire (follows the sound server)");
            } else if (label.contains(QLatin1String("Pulse"), Qt::CaseInsensitive)) {
                label = QStringLiteral("PulseAudio (follows the sound server)");
            } else if (label.contains(QLatin1String("JACK"), Qt::CaseInsensitive)) {
                label = QStringLiteral("JACK (follows the sound server)");
            } else {
                label = QStringLiteral("%1 (follows the sound server)").arg(label);
            }
        } else if (kind == QLatin1String("card")) {
            label = QStringLiteral("%1 (exclusive — may block other apps)").arg(label);
        } else if (reason.contains(QLatin1String("in use"), Qt::CaseInsensitive)) {
            label = QStringLiteral("%1 (in use)").arg(label);
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("value"), value);
        entry.insert(QStringLiteral("label"), label);
        out.push_back(entry);
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

    // For input, prefer the voice daemon's probed list (only devices capture can
    // open). Fall back to raw ALSA enumeration if the daemon isn't installed or
    // can't be reached, so the picker still works.
    bool voiceOk = false;
    QVariantList inputDevices = enumerateVoiceInputDevices(&voiceOk);
    if (!voiceOk) {
        inputDevices = enumerateAudioDevices(QStringLiteral("input"));
    }
    m_inputDeviceOptions = withDefault(inputDevices);
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

void DesktopAssistantKcm::measureInputLevel()
{
    // Briefly sample the input device and report a 0..1 peak so the page can
    // nudge a too-quiet mic (ties into voice#47), via a short `parecord` capture
    // whose peak sample magnitude we compute.
    //
    // KDE-2 / #57, PR 5/5: this used to BLOCK the UI thread on
    // waitForStarted/waitForReadyRead/waitForFinished (~0.4–3s). It is now
    // NON-BLOCKING and void — it spawns the capture, lets a single-shot timer end
    // it after a short window, and emits inputLevelMeasured(level) when done
    // (level == -1 on any failure). The QML "Test" button binds micLevel from
    // that signal. A re-entrancy guard ignores clicks while a capture is running.
    if (m_inputLevelMeasuring) {
        return;
    }
    if (QStandardPaths::findExecutable(QStringLiteral("parecord")).isEmpty()) {
        Q_EMIT inputLevelMeasured(-1.0);
        return;
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

    m_inputLevelMeasuring = true;
    m_statusText = QStringLiteral("Measuring microphone level…");
    Q_EMIT statusTextChanged();

    auto *rec = new QProcess(this);
    auto data = std::make_shared<QByteArray>();
    auto fired = std::make_shared<bool>(false);

    // Single shared completion path: compute the peak from whatever was captured
    // and emit exactly once, then tear the process down. Called from the capture
    // window timer (normal path) and errorOccurred (e.g. parecord missing/failed).
    auto complete = [this, rec, data, fired](bool failed) {
        if (*fired) {
            return;
        }
        *fired = true;
        *data += rec->readAllStandardOutput();
        const double level = failed
            ? -1.0
            : daemonreply::peakLevelFromS16le(*data);
        m_inputLevelMeasuring = false;
        rec->kill();
        rec->deleteLater();
        Q_EMIT inputLevelMeasured(level);
    };

    connect(rec, &QProcess::errorOccurred, this,
            [complete](QProcess::ProcessError) { complete(true); });
    connect(rec, &QProcess::readyReadStandardOutput, this,
            [rec, data]() { *data += rec->readAllStandardOutput(); });

    // End the capture after a short window, then complete on the captured data.
    // peakLevelFromS16le returns -1 for an empty/sub-sample buffer, so a capture
    // that produced nothing is reported as "no level" without a special case.
    QTimer::singleShot(400, this, [complete]() { complete(false); });

    rec->start(QStringLiteral("parecord"), args);
}

void DesktopAssistantKcm::resetWakeDefaults()
{
    m_wakeSensitivity = kVoiceDefaultSensitivity;
    m_wakeEager = kVoiceDefaultWakeEager;
    m_listeningCue.clear();
    flushVoiceConfigWrite();
    Q_EMIT voiceConfigChanged();
    m_statusText = QStringLiteral("Wake-word settings reset to defaults");
    Q_EMIT statusTextChanged();
}

void DesktopAssistantKcm::resetEndpointingDefaults()
{
    m_vadSpeechThreshold = kVoiceDefaultSpeechThreshold;
    m_vadSilenceDurationMs = kVoiceDefaultSilenceDurationMs;
    m_followupTimeoutMs = kVoiceDefaultFollowupTimeoutMs;
    flushVoiceConfigWrite();
    Q_EMIT voiceConfigChanged();
    m_statusText = QStringLiteral("Endpointing settings reset to defaults");
    Q_EMIT statusTextChanged();
}

void DesktopAssistantKcm::resetDeviceDefaults()
{
    m_inputDevice = QStringLiteral("default");
    m_outputDevice = QStringLiteral("default");
    flushVoiceConfigWrite();
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
    flushVoiceConfigWrite();
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
        || snake.startsWith(QLatin1String("delete_knowledge_"))
        || snake == QLatin1String("start_maintenance");

    const QByteArray objectPath = isKnowledge
        ? QByteArrayLiteral("/org/desktopAssistant/Knowledge")
        : QByteArrayLiteral("/org/desktopAssistant/Connections");
    const QByteArray interfaceName = isKnowledge
        ? QByteArrayLiteral("org.desktopAssistant.Knowledge")
        : QByteArrayLiteral("org.desktopAssistant.Connections");

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

    // Build the D-Bus method name + argument list per command. Methods that
    // return a JSON-encoded `CommandResult` produce a string we re-parse in the
    // async handler; the `Ack` commands return an empty signature. `timeoutMs`
    // is bounded per command (KDE-2 / #57) so a wedged daemon can't stall the
    // System Settings UI thread.
    QString method;
    QVariantList callArgs;
    bool returnsJson = false;
    int timeoutMs = DBUS_TIMEOUT_DEFAULT_MS;
    if (snake == QLatin1String("list_connections")) {
        method = QStringLiteral("ListConnections");
        returnsJson = true;
    } else if (snake == QLatin1String("get_purposes")) {
        method = QStringLiteral("GetPurposes");
        returnsJson = true;
    } else if (snake == QLatin1String("list_available_models")) {
        const QString cid = payloadObj.value(QStringLiteral("connection_id")).toString();
        const bool refresh = payloadObj.value(QStringLiteral("refresh")).toBool(false);
        method = QStringLiteral("ListAvailableModels");
        callArgs << cid << refresh;
        returnsJson = true;
        // Model enumeration may reach out to a remote provider; give it room.
        timeoutMs = DBUS_TIMEOUT_MODELS_MS;
    } else if (snake == QLatin1String("create_connection")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        const QString configJson = serializePayloadField(QStringLiteral("config"));
        method = QStringLiteral("CreateConnection");
        callArgs << id << configJson;
    } else if (snake == QLatin1String("update_connection")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        const QString configJson = serializePayloadField(QStringLiteral("config"));
        method = QStringLiteral("UpdateConnection");
        callArgs << id << configJson;
    } else if (snake == QLatin1String("delete_connection")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        const bool force = payloadObj.value(QStringLiteral("force")).toBool(false);
        method = QStringLiteral("DeleteConnection");
        callArgs << id << force;
    } else if (snake == QLatin1String("set_purpose")) {
        const QString purpose = payloadObj.value(QStringLiteral("purpose")).toString();
        const QString configJson = serializePayloadField(QStringLiteral("config"));
        method = QStringLiteral("SetPurpose");
        callArgs << purpose << configJson;
    } else if (snake == QLatin1String("list_knowledge_entries")) {
        const uint limit = static_cast<uint>(
            payloadObj.value(QStringLiteral("limit")).toInt(50));
        const uint offset = static_cast<uint>(
            payloadObj.value(QStringLiteral("offset")).toInt(0));
        const QString tagFilterJson = serializeArrayField(QStringLiteral("tag_filter"));
        method = QStringLiteral("ListEntries");
        callArgs << limit << offset << tagFilterJson;
        returnsJson = true;
    } else if (snake == QLatin1String("get_knowledge_entry")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        method = QStringLiteral("GetEntry");
        callArgs << id;
        returnsJson = true;
    } else if (snake == QLatin1String("search_knowledge_entries")) {
        const QString query = payloadObj.value(QStringLiteral("query")).toString();
        const QString tagFilterJson = serializeArrayField(QStringLiteral("tag_filter"));
        const uint limit = static_cast<uint>(
            payloadObj.value(QStringLiteral("limit")).toInt(50));
        method = QStringLiteral("SearchEntries");
        callArgs << query << tagFilterJson << limit;
        returnsJson = true;
        // A search may scan a large knowledge store.
        timeoutMs = DBUS_TIMEOUT_SEARCH_MS;
    } else if (snake == QLatin1String("create_knowledge_entry")) {
        const QString content = payloadObj.value(QStringLiteral("content")).toString();
        const QString tagsJson = serializeArrayField(QStringLiteral("tags"));
        const QString metadataJson = serializeValueField(QStringLiteral("metadata"));
        method = QStringLiteral("CreateEntry");
        callArgs << content << tagsJson << metadataJson;
        returnsJson = true;
    } else if (snake == QLatin1String("update_knowledge_entry")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        const QString content = payloadObj.value(QStringLiteral("content")).toString();
        const QString tagsJson = serializeArrayField(QStringLiteral("tags"));
        const QString metadataJson = serializeValueField(QStringLiteral("metadata"));
        method = QStringLiteral("UpdateEntry");
        callArgs << id << content << tagsJson << metadataJson;
        returnsJson = true;
    } else if (snake == QLatin1String("delete_knowledge_entry")) {
        const QString id = payloadObj.value(QStringLiteral("id")).toString();
        method = QStringLiteral("DeleteEntry");
        callArgs << id;
    } else if (snake == QLatin1String("start_maintenance")) {
        // Dream-cycle controls: trigger an extraction / consolidation /
        // embedding-recompute pass. Returns immediately with a JSON envelope
        // carrying the background task id; progress arrives via Task* signals
        // and the pass emits Knowledge.EntriesChanged as entries land.
        const QString op = payloadObj.value(QStringLiteral("op")).toString();
        method = QStringLiteral("StartMaintenance");
        callArgs << op;
        returnsJson = true;
    } else {
        fail(QStringLiteral("daemonCall: unsupported command '%1'").arg(snake));
        return;
    }

    // Async dispatch: the watcher fires `handler` on the UI thread when the
    // reply arrives (or times out). The QML callback is invoked exactly once
    // from there — on error with (null, message), on success with (result,
    // null) — preserving the historical callback contract. `callback` is a
    // QJSValue copy captured by value; `this` is the watcher's parent so the
    // handler never outlives the KCM.
    asyncSettingsCall(
        method, callArgs, timeoutMs,
        [this, callback, returnsJson](const QDBusMessage &reply) {
            auto invokeFail = [this, callback](const QString &message) {
                if (!callback.isCallable()) {
                    return;
                }
                QJSEngine *engine = qjsEngine(this);
                QJSValueList argv;
                argv << QJSValue(QJSValue::NullValue);
                argv << (engine ? engine->toScriptValue(message) : QJSValue(message));
                QJSValue cb = callback;
                cb.call(argv);
            };

            if (reply.type() == QDBusMessage::ErrorMessage) {
                invokeFail(daemonreply::dbusErrorMessage(reply.errorName(), reply.errorMessage()));
                return;
            }

            QVariant resultVariant;
            if (returnsJson) {
                const daemonreply::JsonReply parsed = daemonreply::parseJsonReply(reply.arguments());
                if (!parsed.ok) {
                    invokeFail(parsed.error);
                    return;
                }
                resultVariant = parsed.value;
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
        },
        SERVICE, objectPath.constData(), interfaceName.constData());
}

#include "desktopassistantkcm.moc"
