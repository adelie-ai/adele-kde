#include "daemonwsclient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>

DaemonWsClient::DaemonWsClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    // 15 s is generous but matches `ListAvailableModels --refresh` on
    // Bedrock, which needs a round-trip to AWS.
    m_timeout->setInterval(15000);

    connect(m_socket, &QWebSocket::connected, this, &DaemonWsClient::handleConnected);
    connect(m_socket, &QWebSocket::textMessageReceived, this, &DaemonWsClient::handleTextMessage);
    connect(
        m_socket,
        QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
        this,
        &DaemonWsClient::handleError
    );
    connect(m_socket, &QWebSocket::disconnected, this, [this]() {
        if (m_pending) {
            // Only surface this if we hadn't already resolved the request.
            finish({}, m_socket->errorString().isEmpty()
                ? QStringLiteral("WebSocket closed before response")
                : m_socket->errorString());
        }
    });
    connect(m_timeout, &QTimer::timeout, this, &DaemonWsClient::handleTimeout);
}

DaemonWsClient::~DaemonWsClient() = default;

void DaemonWsClient::send(
    const QUrl &url,
    const QString &token,
    const QString &command,
    const QJsonObject &payload,
    std::function<void(const QVariant &, const QString &)> onDone
)
{
    if (m_pending) {
        if (onDone) {
            onDone({}, QStringLiteral("Another request is already in flight"));
        }
        return;
    }

    if (token.trimmed().isEmpty()) {
        if (onDone) {
            onDone({}, QStringLiteral("No WebSocket JWT; is the daemon running on the session bus?"));
        }
        return;
    }

    m_pending = true;
    m_onDone = std::move(onDone);

    // The daemon's api-model encodes commands as `{ snake_case_variant: { ... } }`.
    // Unit variants (`Ping`, `ListConnections`, `GetPurposes`) need the
    // variant key even when the payload is empty, so we always wrap the
    // payload in a JSON object.
    QJsonObject inner = payload;
    QJsonObject envelope;
    m_pendingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    envelope.insert(QStringLiteral("id"), m_pendingId);
    QJsonObject cmd;
    cmd.insert(command, inner);
    envelope.insert(QStringLiteral("command"), cmd);

    m_pendingEnvelope = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));

    QNetworkRequest request(url);
    // desktop-assistant#9 fixed to require `Authorization: Bearer <token>`;
    // no query-string fallback. This header is also what the WS ping test in
    // crates/ws-interface/tests/ping.rs uses.
    request.setRawHeader(
        "Authorization",
        (QStringLiteral("Bearer ") + token.trimmed()).toUtf8()
    );

    m_timeout->start();
    m_socket->open(request);
}

void DaemonWsClient::handleConnected()
{
    if (!m_pendingEnvelope.isEmpty()) {
        m_socket->sendTextMessage(m_pendingEnvelope);
        m_pendingEnvelope.clear();
    }
}

void DaemonWsClient::handleError()
{
    if (!m_pending) {
        return;
    }
    const auto text = m_socket->errorString();
    finish({}, text.isEmpty() ? QStringLiteral("WebSocket transport error") : text);
}

void DaemonWsClient::handleTextMessage(const QString &message)
{
    if (!m_pending) {
        return;
    }
    const auto doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    const auto obj = doc.object();
    if (obj.contains(QStringLiteral("result"))) {
        const auto inner = obj.value(QStringLiteral("result")).toObject();
        if (inner.value(QStringLiteral("id")).toString() != m_pendingId) {
            return;
        }
        const auto result = inner.value(QStringLiteral("result"));
        finish(result.toVariant(), {});
        return;
    }
    if (obj.contains(QStringLiteral("error"))) {
        const auto inner = obj.value(QStringLiteral("error")).toObject();
        if (inner.value(QStringLiteral("id")).toString() != m_pendingId) {
            return;
        }
        const auto text = inner.value(QStringLiteral("error")).toString();
        finish({}, text.isEmpty() ? QStringLiteral("Daemon returned an error") : text);
        return;
    }
    // `event` frames (streaming deltas) are ignored; the KCM never sends
    // `SendMessage` so we shouldn't see them here anyway.
}

void DaemonWsClient::handleTimeout()
{
    if (!m_pending) {
        return;
    }
    finish({}, QStringLiteral("Daemon request timed out"));
}

void DaemonWsClient::finish(const QVariant &result, const QString &error)
{
    if (!m_pending) {
        return;
    }
    m_pending = false;
    m_timeout->stop();
    m_socket->close();
    auto cb = std::move(m_onDone);
    m_onDone = nullptr;
    m_pendingId.clear();
    m_pendingEnvelope.clear();
    if (cb) {
        cb(result, error);
    }
}
