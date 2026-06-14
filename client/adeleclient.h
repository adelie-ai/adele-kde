// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <qqmlregistration.h>

// Opaque FFI handle from desktop-assistant's client-ffi crate (adele_client.h).
// Forward-declared so this header doesn't drag the C header into every includer.
struct AdeleClient;

namespace adele {

/**
 * QML-facing client for desktop-assistant. Owns ONE FFI connection to the daemon
 * (UDS, via client-common's Connector — the same path gtk/tui use) and turns its
 * streamed SignalEvents into Qt signals delivered on the GUI thread, so QML
 * renders live with no polling.
 *
 * Per the repo convention "daemon talks happen from C++, not QML", all transport
 * lives here; QML only calls the Q_INVOKABLEs and connects to the signals.
 *
 * Threading: the FFI signal callback fires on a tokio worker thread. It does not
 * touch this object directly — it posts a queued call to dispatchEvent(), which
 * runs on this object's (GUI) thread and emits the typed signal.
 */
class AdeleDaemon : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)

public:
    explicit AdeleDaemon(QObject *parent = nullptr);
    ~AdeleDaemon() override;

    AdeleDaemon(const AdeleDaemon &) = delete;
    AdeleDaemon &operator=(const AdeleDaemon &) = delete;

    [[nodiscard]] bool isConnected() const
    {
        return m_handle != nullptr;
    }

    /**
     * Connect to the daemon over UDS. Empty paths use the platform defaults
     * ($XDG_RUNTIME_DIR/adelie/{sock,mint.sock}). A second call while already
     * connected is a no-op returning true. Returns false if the daemon is
     * unreachable — the object degrades to disconnected rather than throwing,
     * so a headless / daemon-down session stays usable.
     */
    Q_INVOKABLE bool connectToDaemon(const QString &socketPath = QString(),
                                     const QString &minterSocket = QString());

    /**
     * Set-replace the conversations this client receives live events for. Pass an
     * empty list to unsubscribe from all. Returns false when disconnected or the
     * dispatch fails.
     */
    Q_INVOKABLE bool subscribeConversations(const QStringList &conversationIds);

    /**
     * Send a prompt to a conversation; returns the turn request-id that the
     * streamed events carry, or an empty string on failure / when disconnected.
     */
    Q_INVOKABLE QString sendPrompt(const QString &conversationId, const QString &prompt);

Q_SIGNALS:
    void connectedChanged(bool connected);
    void userMessageAdded(const QString &conversationId, const QString &requestId, const QString &content);
    void chunkReceived(const QString &conversationId, const QString &requestId, const QString &chunk);
    void completed(const QString &conversationId, const QString &requestId, const QString &fullResponse);
    void errorReceived(const QString &conversationId, const QString &requestId, const QString &error);
    void statusReceived(const QString &conversationId, const QString &requestId, const QString &message);
    void titleChanged(const QString &conversationId, const QString &title);
    void conversationListChanged(const QString &conversationId);
    void clientToolCall(const QString &taskId,
                        const QString &conversationId,
                        const QString &toolCallId,
                        const QString &toolName,
                        const QString &argumentsJson);
    void disconnected(const QString &reason);

private:
    // Parses one tagged-JSON event and emits the matching signal. Q_INVOKABLE so
    // the queued call from onSignal() can target it by name; runs on this
    // object's thread. Marked private — it is an internal marshalling hop, not
    // part of the QML surface.
    Q_INVOKABLE void dispatchEvent(const QString &json);

    // C callback handed to the FFI. Fires on a tokio worker thread; it only
    // copies the JSON and posts a queued dispatchEvent() — no QObject access
    // off-thread.
    static void onSignal(const char *json, void *userData);

    ::AdeleClient *m_handle = nullptr;
};

} // namespace adele
