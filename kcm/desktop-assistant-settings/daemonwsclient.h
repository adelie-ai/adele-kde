#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

#include <QtWebSockets/QWebSocket>

class QTimer;

/// Short-lived WebSocket client that round-trips a single daemon `Command` +
/// `CommandResult` over the multi-connection API exposed on
/// `ws://127.0.0.1:11339/ws` (see desktop-assistant/crates/ws-interface).
///
/// The daemon rejects requests that aren't authenticated with
/// `Authorization: Bearer <jwt>`, so we attach the header via
/// `QNetworkRequest` before calling `QWebSocket::open`. Qt's QML `WebSocket`
/// type cannot set request headers, which is why this sits in C++.
///
/// Usage pattern from QML:
///
///     kcm.wsCall("list_connections", {}, function(resultVariant, error) { ... })
///
/// The callback receives `(QVariant result, QString error)`:
///   - on success: `result` is the daemon's `CommandResult` unwrapped one
///     level (e.g. `{ "connections": [...] }` for `ListConnections`);
///   - on failure: `result` is null, `error` carries a user-readable reason.
///
/// Only one request is in flight at a time; callers should queue their own
/// follow-ups from the callback. Requests have a hard timeout so a
/// half-open socket can never wedge the KCM's tab.
class DaemonWsClient : public QObject {
    Q_OBJECT

public:
    explicit DaemonWsClient(QObject *parent = nullptr);
    ~DaemonWsClient() override;

    /// Submit a snake-cased command to the daemon. `command` is the envelope
    /// variant (e.g. `"list_connections"`); `payload` is the inner object
    /// (empty for unit variants like `ping` / `list_connections`). The
    /// callback is a QJSValue invoked exactly once; it receives
    /// `(result, errorMessage)` where `result` is the unwrapped
    /// CommandResult variant on success or `undefined` on failure.
    void send(
        const QUrl &url,
        const QString &token,
        const QString &command,
        const QJsonObject &payload,
        std::function<void(const QVariant &, const QString &)> onDone
    );

    /// True while the client is waiting for a response. QML pages flip a
    /// spinner based on this; the call itself queues nothing so a second
    /// `send()` during an in-flight request immediately fails with a
    /// user-readable error.
    bool isBusy() const { return m_pending; }

private Q_SLOTS:
    void handleConnected();
    void handleError();
    void handleTextMessage(const QString &message);
    void handleTimeout();

private:
    void finish(const QVariant &result, const QString &error);

    QWebSocket *m_socket = nullptr;
    QTimer *m_timeout = nullptr;
    bool m_pending = false;
    QString m_pendingId;
    QString m_pendingEnvelope;
    std::function<void(const QVariant &, const QString &)> m_onDone;
};
