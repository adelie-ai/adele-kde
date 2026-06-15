// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <qqmlregistration.h>

class QDBusServiceWatcher;

namespace adele {

/**
 * QML-facing controller for the voice daemon (repo adelie-ai/voice), which owns
 * the DISTINCT well-known name `org.desktopAssistant.Voice` — separate from the
 * orchestrator the chat core ([`AdeleCore`]) talks to. The chat reducer only
 * knows per-conversation voice *state*; the voice *pipeline* (mic, wake word,
 * TTS engine, installed voices) lives here and is reached over its own session-
 * bus service.
 *
 * This object is glue, mirroring the old Python helper's `voice-*` commands as
 * native QtDBus calls — but signal-driven rather than polled: it subscribes to
 * the daemon's `StateChanged` signal and watches the service name's ownership,
 * so the UI reflects Listening/Processing/Speaking live and disables cleanly
 * (capability degradation) whenever the daemon isn't on the bus.
 *
 * Threading: everything runs on the GUI thread. D-Bus method calls are issued
 * asynchronously (fire-and-forget for actions; pending-call watchers for the
 * few queries) so a slow/absent daemon never blocks the shell.
 */
class VoiceController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // The service has an owner on the bus (installed + running). Gates the whole
    // voice UI: false ⇒ every control hides/disables instead of erroring.
    Q_PROPERTY(bool available READ isAvailable NOTIFY availableChanged)
    // Pipeline state: "Idle" | "Listening" | "Processing" | "Speaking".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // Resident "Hey Adele" wake-word listening.
    Q_PROPERTY(bool enabled READ isEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QString voiceId READ voiceId NOTIFY voiceChanged)
    // Active speaker index for multi-speaker voices; -1 = default/unset.
    Q_PROPERTY(int speakerId READ speakerId NOTIFY voiceChanged)
    // Installed TTS voices: [{voice_id, display_name, language, num_speakers, label}].
    Q_PROPERTY(QVariantList voices READ voices NOTIFY voicesChanged)

public:
    explicit VoiceController(QObject *parent = nullptr);
    ~VoiceController() override;

    VoiceController(const VoiceController &) = delete;
    VoiceController &operator=(const VoiceController &) = delete;

    [[nodiscard]] bool isAvailable() const
    {
        return m_available;
    }
    [[nodiscard]] QString state() const
    {
        return m_state;
    }
    [[nodiscard]] bool isEnabled() const
    {
        return m_enabled;
    }
    [[nodiscard]] QString voiceId() const
    {
        return m_voiceId;
    }
    [[nodiscard]] int speakerId() const
    {
        return m_speakerId;
    }
    [[nodiscard]] QVariantList voices() const
    {
        return m_voices;
    }

    /**
     * Begin watching the service name + subscribe to `StateChanged`, and seed
     * the initial state when the daemon is already up. Idempotent: safe to call
     * once from `Component.onCompleted`. A no-op when the session bus is
     * unavailable (headless) — the controller simply stays `available == false`.
     */
    Q_INVOKABLE void start();

    /**
     * Start a dictation turn (push-to-talk). With a non-empty `conversationId`
     * the daemon dictates the spoken prompt + reply INTO that conversation
     * (voice#24); empty falls back to the daemon's own voice session. Barges in
     * (stops playback) first when the daemon is Speaking.
     */
    Q_INVOKABLE void pushToTalk(const QString &conversationId = QString());
    /** Abort an in-flight capture/processing turn — returns the daemon to Idle. */
    Q_INVOKABLE void stopListening();
    /** Cut TTS playback (barge-in). */
    Q_INVOKABLE void stopSpeaking();
    /** Toggle the resident "Hey Adele" wake word. Optimistic + reconciled. */
    Q_INVOKABLE void setEnabled(bool enabled);
    /** Change the active TTS voice; `speaker` < 0 means default/single-speaker. */
    Q_INVOKABLE void setVoice(const QString &voiceId, int speaker);
    /** Speak `text` via the daemon's TTS engine (routes the core's Speak event). */
    Q_INVOKABLE void sayText(const QString &text);
    /** Re-fetch the installed-voice list. */
    Q_INVOKABLE void refreshVoices();

    /**
     * Build the voice-switcher label "Name (lang)". Pure + static so it's
     * unit-testable without a bus; used when materializing `voices`.
     */
    [[nodiscard]] static QString voiceLabel(const QString &displayName, const QString &language);

Q_SIGNALS:
    void availableChanged(bool available);
    void stateChanged(const QString &state);
    void enabledChanged(bool enabled);
    void voiceChanged();
    void voicesChanged();

private Q_SLOTS:
    // Wired to the daemon's `StateChanged(s)` D-Bus signal (string-based connect
    // needs a real slot). Runs on the GUI thread.
    void handleStateChanged(const QString &state);

private:
    void setAvailable(bool available);
    void setState(const QString &state);
    // Pull GetState/GetEnabled/GetVoice + the voice list once the service is up.
    void seedState();
    void fetchVoice();

    bool m_available = false;
    QString m_state = QStringLiteral("Idle");
    bool m_enabled = false;
    QString m_voiceId;
    int m_speakerId = -1;
    QVariantList m_voices;

    QDBusServiceWatcher *m_watcher = nullptr;
    bool m_started = false;
};

} // namespace adele
