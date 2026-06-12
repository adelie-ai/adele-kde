#include "daemonreply.h"

#include <algorithm>

#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>

namespace daemonreply {

namespace {
int clampTrait(int value)
{
    return std::clamp(value, 0, 4);
}
} // namespace

JsonReply parseJsonReply(const QList<QVariant> &args)
{
    JsonReply out;
    if (args.isEmpty()) {
        out.error = QStringLiteral("D-Bus reply missing JSON payload");
        return out;
    }

    const QString json = args.first().toString();
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        out.error = QStringLiteral("Failed to parse daemon reply: %1").arg(parseError.errorString());
        return out;
    }

    out.ok = true;
    out.value = doc.toVariant();
    return out;
}

QString dbusErrorMessage(const QString &errorName, const QString &errorMessage)
{
    if (!errorMessage.isEmpty()) {
        return errorMessage;
    }
    if (!errorName.isEmpty()) {
        return errorName;
    }
    return QStringLiteral("D-Bus call failed");
}

PersistenceReply parsePersistenceReply(const QList<QVariant> &args)
{
    PersistenceReply out;
    if (args.size() < 4) {
        out.error = QStringLiteral("Unexpected GetPersistenceSettings reply");
        return out;
    }
    out.ok = true;
    out.gitEnabled = args[0].toBool();
    out.gitRemoteUrl = args[1].toString();
    out.gitRemoteName = args[2].toString();
    out.gitPushOnUpdate = args[3].toBool();
    return out;
}

DatabaseReply parseDatabaseReply(const QList<QVariant> &args)
{
    DatabaseReply out;
    if (args.size() < 2) {
        out.error = QStringLiteral("Unexpected GetDatabaseSettings reply");
        return out;
    }
    out.ok = true;
    out.dbUrl = args[0].toString();
    out.dbMaxConnections = args[1].toInt();
    return out;
}

BackendTasksReply parseBackendTasksReply(const QList<QVariant> &args)
{
    BackendTasksReply out;
    if (args.size() < 6) {
        out.error = QStringLiteral("Unexpected GetBackendTasksSettings reply");
        return out;
    }
    out.ok = true;
    out.llmConnector = args[1].toString();
    out.llmModel = args[2].toString();
    out.llmBaseUrl = args[3].toString();
    out.dreamingEnabled = args[4].toBool();
    out.dreamingIntervalSecs = static_cast<int>(args[5].toULongLong());
    out.archiveAfterDays = args.size() > 6 ? static_cast<int>(args[6].toUInt()) : 0;
    return out;
}

WsAuthReply parseWsAuthReply(const QList<QVariant> &args)
{
    WsAuthReply out;
    if (args.size() < 6) {
        return out; // best-effort: keep defaults, no error (historical behaviour)
    }
    out.ok = true;
    out.methods = args[0].toStringList();
    out.oidcIssuer = args[1].toString();
    out.oidcAuthEndpoint = args[2].toString();
    out.oidcTokenEndpoint = args[3].toString();
    out.oidcClientId = args[4].toString();
    out.oidcScopes = args[5].toString();
    if (out.oidcScopes.isEmpty()) {
        out.oidcScopes = QStringLiteral("openid profile email");
    }
    return out;
}

PersonalityReply parsePersonalityReply(const QList<QVariant> &args, int baseFields)
{
    PersonalityReply out;
    constexpr int kTraitCount = 7;
    if (args.size() < baseFields + kTraitCount) {
        return out; // pre-#226 daemon (or shorter reply): keep defaults
    }
    out.present = true;
    out.professionalism = clampTrait(args[baseFields + 0].toInt());
    out.warmth = clampTrait(args[baseFields + 1].toInt());
    out.directness = clampTrait(args[baseFields + 2].toInt());
    out.enthusiasm = clampTrait(args[baseFields + 3].toInt());
    out.humor = clampTrait(args[baseFields + 4].toInt());
    out.sarcasm = clampTrait(args[baseFields + 5].toInt());
    out.pretentiousness = clampTrait(args[baseFields + 6].toInt());
    return out;
}

} // namespace daemonreply
