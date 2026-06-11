#pragma once

// Pure, bus-free helpers for turning a D-Bus reply's argument list into the
// shapes the KCM hands back to QML. These are deliberately free functions that
// take a plain `QList<QVariant>` (the reply's `arguments()`) rather than a
// `QDBusMessage`, so the C++ test target can exercise them without a live
// session bus. The async D-Bus plumbing in the KCM (KDE-2 / #57) builds its
// callbacks on top of these.

#include <QList>
#include <QString>
#include <QVariant>

namespace daemonreply {

// Result of parsing a daemon reply that carries a JSON `CommandResult` string
// in its first argument. Exactly one of `ok`/error applies: on success `ok` is
// true and `value` holds the decoded JSON (as a QVariant tree); on failure `ok`
// is false and `error` describes why (missing payload / malformed JSON).
struct JsonReply {
    bool ok = false;
    QVariant value;
    QString error;
};

// Parse the first argument of a JSON-returning daemon reply. Mirrors the
// historical inline logic in `daemonCall`: an empty argument list is "missing
// JSON payload", a non-parseable string is a parse error, otherwise the decoded
// document is returned as a QVariant.
JsonReply parseJsonReply(const QList<QVariant> &args);

// Format a human-readable error string from a D-Bus error name + message,
// falling back to a generic message when both are empty. Kept here (rather than
// inline in the KCM) so error formatting is unit-testable and shared between the
// sync and async call paths.
QString dbusErrorMessage(const QString &errorName, const QString &errorMessage);

} // namespace daemonreply
