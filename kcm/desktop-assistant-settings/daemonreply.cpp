#include "daemonreply.h"

#include <algorithm>

#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaType>
#include <QStringList>

namespace daemonreply {

namespace {
// Does QVariant `v` hold the concrete runtime type the D-Bus signature char
// `sig` denotes? QtDBus flattens a struct reply into one QVariant per field,
// each carrying its concrete Qt type, so the field types are checkable even
// though the wire signature itself is gone. Returns false for an unsupported
// signature char (a guard against a stale/typo'd expected signature).
bool variantMatchesSignature(const QVariant &v, QChar sig)
{
    switch (sig.toLatin1()) {
    case 's':
        return v.metaType().id() == QMetaType::QString;
    case 'b':
        return v.metaType().id() == QMetaType::Bool;
    case 'u':
        return v.metaType().id() == QMetaType::UInt;
    case 'i':
        return v.metaType().id() == QMetaType::Int;
    case 'd':
        return v.metaType().id() == QMetaType::Double;
    default:
        return false;
    }
}

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

PersonalityConfig parsePersonalityConfig(const QList<QVariant> &args,
                                         const QString &expectedBaseSignature,
                                         int traitCount)
{
    PersonalityConfig out;
    const int baseFields = static_cast<int>(expectedBaseSignature.size());
    const int expectedSize = baseFields + traitCount;

    // Arity must match EXACTLY: a longer reply means the daemon appended fields
    // after the personality block (so our trailing-block indices would read the
    // wrong fields); a shorter one means it predates the block (pre-#226) or the
    // schema diverged. Either way the positional read is unsafe.
    if (args.size() != expectedSize) {
        out.error = QStringLiteral(
            "Personality config: GetConfig returned %1 fields, expected %2 — "
            "daemon config schema mismatch; keeping defaults")
            .arg(args.size())
            .arg(expectedSize);
        return out;
    }

    // Every leading field's runtime type must match the expected base signature.
    for (int i = 0; i < baseFields; ++i) {
        if (!variantMatchesSignature(args[i], expectedBaseSignature[i])) {
            out.error = QStringLiteral(
                "Personality config: GetConfig field %1 has unexpected type "
                "(expected '%2'); daemon config schema mismatch; keeping defaults")
                .arg(i)
                .arg(expectedBaseSignature[i]);
            return out;
        }
    }

    // The trailing personality block must be exactly `traitCount` u32 fields.
    for (int i = 0; i < traitCount; ++i) {
        if (args[baseFields + i].metaType().id() != QMetaType::UInt) {
            out.error = QStringLiteral(
                "Personality config: trait field %1 is not a u32; daemon config "
                "schema mismatch; keeping defaults")
                .arg(i);
            return out;
        }
    }

    out.signatureOk = true;
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
