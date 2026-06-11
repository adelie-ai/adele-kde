#include "daemonreply.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace daemonreply {

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

} // namespace daemonreply
