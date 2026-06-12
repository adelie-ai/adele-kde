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

// --- Personality config signature validation (KDE-8 / #63) -------------------
// The seven personality traits have no granular D-Bus getter, so the KCM reads
// them by fixed positional index out of the flattened GetConfig reply (QtDBus
// flattens the returned ConfigData struct into one QVariant per field). If the
// daemon ever inserts a field *before* the personality block, every index shifts
// and the traits silently read plausible-looking garbage (the 0..4-clamped ints
// make this especially insidious). To detect that, we validate the runtime type
// of every reply element against the expected ConfigData signature before
// indexing: the leading fields must match `expectedBaseSignature` (a D-Bus type
// string, e.g. "sssb...") AND the reply must end in exactly `traitCount`
// unsigned-int trait fields, with no extra trailing args. On any mismatch the
// caller keeps its built-in defaults and surfaces a clear status rather than
// reading garbage. The same `ok` guard gates the positional SetConfig push.
//
// Supported signature chars (the only types ConfigData uses): s (QString),
// b (bool), u (u32 -> uint), i (i32 -> int), d (f64 -> double). Any other char
// is treated as a programming error and fails validation.
struct PersonalityConfig {
    // signatureOk: the reply matched the expected base signature + trailing
    // unsigned trait block exactly. Only then are the trait values trustworthy
    // (and only then may the positional SetConfig patch be sent).
    bool signatureOk = false;
    // A human-readable reason when signatureOk is false (arity / type mismatch),
    // suitable for surfacing in statusText. Empty when signatureOk is true.
    QString error;
    // Parsed 0..4-clamped trait values, valid only when signatureOk is true.
    int professionalism = 0;
    int warmth = 0;
    int directness = 0;
    int enthusiasm = 0;
    int humor = 0;
    int sarcasm = 0;
    int pretentiousness = 0;
};

// Validate `args` against `expectedBaseSignature` (the leading ConfigData
// fields) followed by exactly `traitCount` u32 personality fields, then parse
// the traits. See PersonalityConfig.
PersonalityConfig parsePersonalityConfig(const QList<QVariant> &args,
                                         const QString &expectedBaseSignature,
                                         int traitCount);

} // namespace daemonreply
