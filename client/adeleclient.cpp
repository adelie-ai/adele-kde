// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adeleclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QMetaObject>

#include "adele_client.h" // generated C ABI for desktop-assistant's client-ffi

namespace adele {

AdeleDaemon::AdeleDaemon(QObject *parent)
    : QObject(parent)
{
}

AdeleDaemon::~AdeleDaemon()
{
    if (m_handle) {
        // Drops the tokio runtime (joins its workers, stopping the signal pump)
        // and the Connector (closing the daemon connection). A dispatchEvent()
        // already queued for a now-dying object is dropped by Qt.
        adele_client_free(m_handle);
        m_handle = nullptr;
    }
}

bool AdeleDaemon::connectToDaemon(const QString &socketPath, const QString &minterSocket)
{
    if (m_handle) {
        return true; // already connected
    }

    // Keep the UTF-8 buffers alive across the FFI call; pass NULL for empties so
    // the FFI applies its platform defaults.
    const QByteArray sock = socketPath.toUtf8();
    const QByteArray mint = minterSocket.toUtf8();
    m_handle = adele_client_connect(socketPath.isEmpty() ? nullptr : sock.constData(),
                                    minterSocket.isEmpty() ? nullptr : mint.constData());

    if (!m_handle) {
        Q_EMIT connectedChanged(false);
        return false;
    }

    adele_client_start_signals(m_handle, &AdeleDaemon::onSignal, this);
    Q_EMIT connectedChanged(true);
    return true;
}

bool AdeleDaemon::subscribeConversations(const QStringList &conversationIds)
{
    if (!m_handle) {
        return false;
    }
    QJsonArray ids;
    for (const QString &id : conversationIds) {
        ids.append(id);
    }
    const QByteArray json = QJsonDocument(ids).toJson(QJsonDocument::Compact);
    return adele_client_subscribe_conversations(m_handle, json.constData());
}

QString AdeleDaemon::sendPrompt(const QString &conversationId, const QString &prompt)
{
    if (!m_handle) {
        return {};
    }
    const QByteArray conv = conversationId.toUtf8();
    const QByteArray msg = prompt.toUtf8();
    char *requestId = adele_client_send_prompt(m_handle, conv.constData(), msg.constData());
    if (!requestId) {
        return {};
    }
    const QString result = QString::fromUtf8(requestId);
    adele_string_free(requestId);
    return result;
}

void AdeleDaemon::onSignal(const char *json, void *userData)
{
    // Worker-thread context. Copy the JSON into a QString and hop to the GUI
    // thread; do not touch the QObject here beyond posting the queued call.
    auto *self = static_cast<AdeleDaemon *>(userData);
    if (!self || !json) {
        return;
    }
    QMetaObject::invokeMethod(self,
                              "dispatchEvent",
                              Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromUtf8(json)));
}

void AdeleDaemon::dispatchEvent(const QString &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) {
        return; // malformed / non-object — ignore rather than crash
    }
    const QJsonObject obj = doc.object();
    const QString kind = obj.value(QStringLiteral("kind")).toString();

    const auto str = [&obj](const char *key) -> QString {
        return obj.value(QLatin1String(key)).toString();
    };

    if (kind == QLatin1String("user_message_added")) {
        Q_EMIT userMessageAdded(str("conversation_id"), str("request_id"), str("content"));
    } else if (kind == QLatin1String("chunk")) {
        Q_EMIT chunkReceived(str("conversation_id"), str("request_id"), str("chunk"));
    } else if (kind == QLatin1String("complete")) {
        Q_EMIT completed(str("conversation_id"), str("request_id"), str("full_response"));
    } else if (kind == QLatin1String("error")) {
        Q_EMIT errorReceived(str("conversation_id"), str("request_id"), str("error"));
    } else if (kind == QLatin1String("status")) {
        Q_EMIT statusReceived(str("conversation_id"), str("request_id"), str("message"));
    } else if (kind == QLatin1String("title_changed")) {
        Q_EMIT titleChanged(str("conversation_id"), str("title"));
    } else if (kind == QLatin1String("conversation_list_changed")) {
        Q_EMIT conversationListChanged(str("conversation_id"));
    } else if (kind == QLatin1String("client_tool_call")) {
        // `arguments` rides through as a structured JSON value; re-serialize it
        // compactly so QML receives valid JSON (not a Qt debug rendering).
        const QJsonValue args = obj.value(QStringLiteral("arguments"));
        QJsonDocument argsDoc;
        if (args.isArray()) {
            argsDoc.setArray(args.toArray());
        } else {
            argsDoc.setObject(args.toObject());
        }
        Q_EMIT clientToolCall(str("task_id"),
                              str("conversation_id"),
                              str("tool_call_id"),
                              str("tool_name"),
                              QString::fromUtf8(argsDoc.toJson(QJsonDocument::Compact)));
    } else if (kind == QLatin1String("disconnected")) {
        Q_EMIT disconnected(str("reason"));
    }
    // Unknown kinds are intentionally ignored (forward-compatible with new
    // daemon events the chat surface doesn't yet handle).
}

} // namespace adele
