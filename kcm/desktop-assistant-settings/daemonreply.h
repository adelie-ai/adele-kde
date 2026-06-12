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

// --- Section parsers for load() (KDE-2 / #57) --------------------------------
// The async load() path fires each Get* read on its own watcher; these free
// functions turn the reply's flat `arguments()` list into a typed result so the
// per-section handler can update just its fields. Each carries an `ok` flag and,
// when it fails the arity/shape check, an `error` string mirroring the
// historical "Unexpected Get<X> reply" message. Bus-free for unit testing.

struct PersistenceReply {
    bool ok = false;
    QString error;
    bool gitEnabled = false;
    QString gitRemoteUrl;
    QString gitRemoteName;
    bool gitPushOnUpdate = false;
};
// GetPersistenceSettings -> (b enabled, s remote_url, s remote_name, b push).
PersistenceReply parsePersistenceReply(const QList<QVariant> &args);

struct DatabaseReply {
    bool ok = false;
    QString error;
    QString dbUrl;
    int dbMaxConnections = 0;
};
// GetDatabaseSettings -> (s url, u max_connections).
DatabaseReply parseDatabaseReply(const QList<QVariant> &args);

struct BackendTasksReply {
    bool ok = false;
    QString error;
    // Pass-through LLM fields (echoed back unchanged by the setter; no UI binds
    // them). args[0] (has_separate_llm) is intentionally ignored.
    QString llmConnector;
    QString llmModel;
    QString llmBaseUrl;
    bool dreamingEnabled = false;
    int dreamingIntervalSecs = 0;
    int archiveAfterDays = 0;
};
// GetBackendTasksSettings -> (b has_separate_llm, s connector, s model,
// s base_url, b dreaming_enabled, t dreaming_interval, [u archive_after_days]).
BackendTasksReply parseBackendTasksReply(const QList<QVariant> &args);

struct WsAuthReply {
    // Unlike the others this is best-effort: the historical load() updated the
    // fields only when the arity matched and otherwise silently kept defaults
    // (no error recorded). `ok` reflects whether the fields were populated.
    bool ok = false;
    QStringList methods;
    QString oidcIssuer;
    QString oidcAuthEndpoint;
    QString oidcTokenEndpoint;
    QString oidcClientId;
    QString oidcScopes;
};
// GetWsAuthSettings -> (as methods, s issuer, s auth_ep, s token_ep,
// s client_id, s scopes). Defaults oidcScopes to "openid profile email" when
// the reply leaves it empty.
WsAuthReply parseWsAuthReply(const QList<QVariant> &args);

struct PersonalityReply {
    // present == false means the daemon predates the personality block
    // (desktop-assistant#226) or the reply failed the signature/arity guard;
    // the caller keeps its built-in defaults. Traits are clamped to 0..4.
    bool present = false;
    int professionalism = 0;
    int warmth = 0;
    int directness = 0;
    int enthusiasm = 0;
    int humor = 0;
    int sarcasm = 0;
    int pretentiousness = 0;
};
// GetConfig flattens ConfigData into one positional arg per field; the seven
// personality u32s are the trailing block after `baseFields` leading fields.
// Reads them by index only when the arity is exactly baseFields+7 (so a daemon
// field inserted before the block can't shift garbage into the traits — the
// stricter equality guard hardened further client-side in KDE-8 / #63).
PersonalityReply parsePersonalityReply(const QList<QVariant> &args, int baseFields);

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
// This is the stricter superset used by the async load() personality handler
// (it validates the leading-field *types*, not just arity, unlike
// parsePersonalityReply which only arity-checks).
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

// --- Voice service replies (KDE-2 / #57, PR 4/5) -----------------------------
// The voice daemon's GetVoice returns (s voice_id, i speaker_id), speaker -1
// when unset. QtDBus usually flattens that struct into two positional args, but
// some bindings wrap it in a single QDBusArgument struct. This helper handles
// the flat case (two-or-more plain QVariants) so the async voice-load handler
// can parse it without a bus; the wrapped single-QDBusArgument case still needs
// live demarshalling and is handled inline at the call site.
struct VoiceSelectionReply {
    // ok == true only when the flat form was present and parsed. false means the
    // caller should try the wrapped QDBusArgument form (or keep current state).
    bool ok = false;
    QString voiceId;
    int speaker = -1;
};
// Parse the flat (si) form of GetVoice's reply arguments. Returns ok == false
// when fewer than two plain args are present (e.g. the wrapped struct form).
VoiceSelectionReply parseVoiceSelectionReply(const QList<QVariant> &args);

} // namespace daemonreply
