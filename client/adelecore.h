// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <qqmlregistration.h>

// Opaque handle from the client-ui-ffi crate (adele_client_core.h). Forward-
// declared so this header doesn't pull the C header into every includer.
struct Core;

namespace adele {

/**
 * QML-facing core for the Adelie chat clients. Owns ONE instance of the shared
 * Rust core (`libadele_client_core`), which itself owns the `client-ui-common`
 * reducer (the same WindowState state machine gtk/tui run) plus a `client-common`
 * Connector in D-Bus mode (the org.desktopAssistant bridge). All model,
 * controller, and transport logic lives in Rust; this object is **glue**: it
 * forwards user intents to the core and turns the core's pushed view-events into
 * a Qt signal delivered on the GUI thread.
 *
 * Per the repo convention "daemon talks happen from C++, not QML", QML only calls
 * the Q_INVOKABLEs and reacts to viewEvent.
 *
 * Threading: the core's view-event callback fires on a Rust worker thread. It
 * does not touch this object directly — it posts a queued call to
 * dispatchEvent(), which runs on this object's (GUI) thread and emits viewEvent.
 */
class AdeleCore : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)

public:
    explicit AdeleCore(QObject *parent = nullptr);
    ~AdeleCore() override;

    AdeleCore(const AdeleCore &) = delete;
    AdeleCore &operator=(const AdeleCore &) = delete;

    [[nodiscard]] bool isConnected() const
    {
        return m_connected;
    }

    /**
     * Connect to the daemon. `transport` = "dbus" (default) | "uds" | "ws";
     * `address` = the UDS socket path or WS url (empty = platform default),
     * ignored for D-Bus. Fire-and-forget: the outcome arrives later as a
     * `connected` / `connect_error` viewEvent, never a return value, so a
     * daemon-down session never blocks the GUI.
     */
    Q_INVOKABLE void connectToDaemon(const QString &transport = QStringLiteral("dbus"),
                                     const QString &address = QString());

    /** Send a prompt into the open conversation. While a reply streams the core
     *  queues it instead of sending immediately (surfaced via the
     *  `queued_messages` / `composer_text` view-events). */
    Q_INVOKABLE void sendPrompt(const QString &text);
    /** Check out queued message `index` into the composer to edit it (up-arrow
     *  recall / a chip's edit affordance). A negative index is ignored. */
    Q_INVOKABLE void editQueued(int index);
    /** Remove queued message `index` (a chip's x) without sending it. A negative
     *  index is ignored. */
    Q_INVOKABLE void removeQueued(int index);
    /** Abandon an in-progress queued-message edit: the checked-out item returns
     *  to the queue unchanged and the composer clears. A no-op when not editing. */
    Q_INVOKABLE void cancelQueuedEdit();
    /** Open (load) a conversation by id. */
    Q_INVOKABLE void selectConversation(const QString &conversationId);
    /** Create a new conversation and open it. */
    Q_INVOKABLE void newConversation();
    /** Delete a conversation by id. */
    Q_INVOKABLE void deleteConversation(const QString &conversationId);
    /** Set the `You:` (voice input) state for a conversation. */
    Q_INVOKABLE void setVoiceIn(const QString &conversationId, bool enabled);
    /** Set the `Adele:` (voice output) level: "disabled" | "on_demand" | "always". */
    Q_INVOKABLE void setAdeleOutput(const QString &conversationId, const QString &level);
    /** Stage (or clear) a per-message model override. Empty connectionId/modelId
     *  clears it; effort is "low" | "medium" | "high" or empty. */
    Q_INVOKABLE void selectModel(const QString &connectionId, const QString &modelId,
                                 const QString &effort = QString());
    /** Request cancellation of a background task by id. */
    Q_INVOKABLE void cancelTask(const QString &taskId);
    /** Fetch a background task's log page (arrives later as a `task_logs` viewEvent). */
    Q_INVOKABLE void fetchTaskLogs(const QString &taskId);

Q_SIGNALS:
    void connectedChanged(bool connected);

    /**
     * One pushed view-update from the core. `type` is the event tag (e.g.
     * "conversations", "load_conversation", "chunk", "complete", "status"); `data`
     * is the full event object (the ffi crate's view_event.rs schema). QML
     * applies it as a delta to its render state — the reducer already decided
     * what changed, so there is no controller logic on the QML side.
     */
    void viewEvent(const QString &type, const QVariantMap &data);

private:
    // Parse one view-event JSON and emit viewEvent. Q_INVOKABLE so the queued
    // call from onViewEvent() can target it by name; runs on this object's thread.
    Q_INVOKABLE void dispatchEvent(const QString &json);

    // C callback handed to the core. Fires on a Rust worker thread; it only
    // copies the JSON and posts a queued dispatchEvent() — no QObject access
    // off-thread. Arg order matches the C ABI: (user_data, json).
    static void onViewEvent(void *userData, const char *json);

    ::Core *m_handle = nullptr;
    bool m_connected = false;
};

} // namespace adele
