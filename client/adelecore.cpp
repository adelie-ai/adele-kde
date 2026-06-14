// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adelecore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QMetaObject>

#include "adele_client_core.h" // generated C ABI for client-ui-ffi

namespace adele {

AdeleCore::AdeleCore(QObject *parent)
    : QObject(parent)
{
    // Create the core immediately (builds the Rust runtime + reducer actor).
    // Connecting to the daemon is a separate, explicit step (connectToDaemon).
    m_handle = adele_core_new(&AdeleCore::onViewEvent, this);
}

AdeleCore::~AdeleCore()
{
    if (m_handle) {
        // Drops the runtime (joins workers, stopping the signal pump) and the
        // Connector (closing the daemon connection). A dispatchEvent() already
        // queued for a now-dying object is dropped by Qt.
        adele_core_free(m_handle);
        m_handle = nullptr;
    }
}

void AdeleCore::connectToDaemon(const QString &transport, const QString &address)
{
    if (!m_handle) {
        return;
    }
    // Keep the UTF-8 buffers alive across the FFI call. Empty strings are fine:
    // the core treats an empty transport as "dbus" and an empty address as the
    // platform default.
    const QByteArray t = transport.toUtf8();
    const QByteArray a = address.toUtf8();
    adele_core_connect(m_handle, t.constData(), a.constData());
}

void AdeleCore::sendPrompt(const QString &text)
{
    if (!m_handle) {
        return;
    }
    const QByteArray t = text.toUtf8();
    adele_core_send_prompt(m_handle, t.constData());
}

void AdeleCore::selectConversation(const QString &conversationId)
{
    if (!m_handle) {
        return;
    }
    const QByteArray c = conversationId.toUtf8();
    adele_core_select_conversation(m_handle, c.constData());
}

void AdeleCore::newConversation()
{
    if (!m_handle) {
        return;
    }
    adele_core_new_conversation(m_handle);
}

void AdeleCore::deleteConversation(const QString &conversationId)
{
    if (!m_handle) {
        return;
    }
    const QByteArray c = conversationId.toUtf8();
    adele_core_delete_conversation(m_handle, c.constData());
}

void AdeleCore::setVoiceIn(const QString &conversationId, bool enabled)
{
    if (!m_handle) {
        return;
    }
    const QByteArray c = conversationId.toUtf8();
    adele_core_set_voice_in(m_handle, c.constData(), enabled);
}

void AdeleCore::setAdeleOutput(const QString &conversationId, const QString &level)
{
    if (!m_handle) {
        return;
    }
    const QByteArray c = conversationId.toUtf8();
    const QByteArray l = level.toUtf8();
    adele_core_set_adele_output(m_handle, c.constData(), l.constData());
}

void AdeleCore::onViewEvent(void *userData, const char *json)
{
    // Worker-thread context. Copy the JSON into a QString and hop to the GUI
    // thread; do not touch the QObject here beyond posting the queued call.
    auto *self = static_cast<AdeleCore *>(userData);
    if (!self || !json) {
        return;
    }
    QMetaObject::invokeMethod(self,
                              "dispatchEvent",
                              Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromUtf8(json)));
}

void AdeleCore::dispatchEvent(const QString &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) {
        return; // malformed / non-object — ignore rather than crash
    }
    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (type.isEmpty()) {
        return; // no tag — nothing to route
    }

    // Track the convenience `connected` property from the lifecycle events the
    // core emits (the reducer also drives send-sensitivity via its own events).
    if (type == QLatin1String("connected")) {
        if (!m_connected) {
            m_connected = true;
            Q_EMIT connectedChanged(true);
        }
    } else if (type == QLatin1String("client_cleared") || type == QLatin1String("connect_error")) {
        if (m_connected) {
            m_connected = false;
            Q_EMIT connectedChanged(false);
        }
    }

    Q_EMIT viewEvent(type, obj.toVariantMap());
}

} // namespace adele
