// SPDX-License-Identifier: AGPL-3.0-or-later
#include "voicecontroller.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QVariant>
#include <QVariantMap>

namespace adele {

namespace {
// The voice daemon's well-known name / object / interface (repo adelie-ai/voice,
// crates/dbus-interface). DISTINCT from the orchestrator's `org.desktopAssistant`.
const QString kService = QStringLiteral("org.desktopAssistant.Voice");
const QString kPath = QStringLiteral("/org/desktopAssistant/Voice");
const QString kIface = QStringLiteral("org.desktopAssistant.Voice");

QDBusMessage voiceCall(const QString &method)
{
    return QDBusMessage::createMethodCall(kService, kPath, kIface, method);
}
} // namespace

VoiceController::VoiceController(QObject *parent)
    : QObject(parent)
{
}

VoiceController::~VoiceController() = default;

QString VoiceController::voiceLabel(const QString &displayName, const QString &language)
{
    if (language.isEmpty()) {
        return displayName;
    }
    return displayName + QStringLiteral(" (") + language + QStringLiteral(")");
}

void VoiceController::start()
{
    if (m_started) {
        return;
    }
    m_started = true;

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        // Headless / no session bus: stay unavailable and degrade. The voice UI
        // simply hides; the rest of the chat is unaffected.
        return;
    }

    // Track the name's ownership so the UI lights up / disables as the daemon
    // starts and stops — no polling.
    m_watcher = new QDBusServiceWatcher(kService, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, [this](const QString &) {
        setAvailable(true);
        seedState();
    });
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &) {
        setAvailable(false);
        // The pipeline is gone — clear the live state so the chip/ring collapse.
        setState(QStringLiteral("Idle"));
    });

    // Live pipeline state (Listening/Processing/Speaking) via the daemon's
    // StateChanged signal — string-based connect needs the real slot below.
    bus.connect(kService, kPath, kIface, QStringLiteral("StateChanged"), this,
                SLOT(handleStateChanged(QString)));

    // Seed from the current owner, if any. isServiceRegistered is a quick local
    // bus-daemon round-trip done once at startup (the watcher covers changes).
    QDBusConnectionInterface *iface = bus.interface();
    const bool registered = iface && iface->isServiceRegistered(kService).value();
    setAvailable(registered);
    if (registered) {
        seedState();
    }
}

void VoiceController::setAvailable(bool available)
{
    if (m_available == available) {
        return;
    }
    m_available = available;
    Q_EMIT availableChanged(m_available);
}

void VoiceController::setState(const QString &state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged(m_state);
}

void VoiceController::handleStateChanged(const QString &state)
{
    setState(state);
}

void VoiceController::seedState()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }

    // GetState -> s
    auto *stateWatcher = new QDBusPendingCallWatcher(bus.asyncCall(voiceCall(QStringLiteral("GetState"))), this);
    connect(stateWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusMessage reply = call->reply();
        if (reply.type() == QDBusMessage::ReplyMessage) {
            setState(reply.arguments().value(0).toString());
        }
    });

    // GetEnabled -> b
    auto *enabledWatcher = new QDBusPendingCallWatcher(bus.asyncCall(voiceCall(QStringLiteral("GetEnabled"))), this);
    connect(enabledWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusMessage reply = call->reply();
        if (reply.type() == QDBusMessage::ReplyMessage) {
            const bool enabled = reply.arguments().value(0).toBool();
            if (m_enabled != enabled) {
                m_enabled = enabled;
                Q_EMIT enabledChanged(m_enabled);
            }
        }
    });

    fetchVoice();
    refreshVoices();
}

void VoiceController::fetchVoice()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    // GetVoice -> (s, i): two reply args (voice_id, speaker_id).
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(voiceCall(QStringLiteral("GetVoice"))), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusMessage reply = call->reply();
        if (reply.type() != QDBusMessage::ReplyMessage) {
            return;
        }
        const QString voiceId = reply.arguments().value(0).toString();
        const int speaker = reply.arguments().value(1).toInt();
        if (voiceId != m_voiceId || speaker != m_speakerId) {
            m_voiceId = voiceId;
            m_speakerId = speaker;
            Q_EMIT voiceChanged();
        }
    });
}

void VoiceController::refreshVoices()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    // ListVoices -> a(sssu): one reply arg, an array of (voice_id, display_name,
    // language, num_speakers) structs.
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(voiceCall(QStringLiteral("ListVoices"))), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusMessage reply = call->reply();
        if (reply.type() != QDBusMessage::ReplyMessage) {
            return;
        }
        const QDBusArgument arg = reply.arguments().value(0).value<QDBusArgument>();
        QVariantList out;
        arg.beginArray();
        while (!arg.atEnd()) {
            arg.beginStructure();
            QString voiceId;
            QString displayName;
            QString language;
            uint numSpeakers = 1;
            arg >> voiceId >> displayName >> language >> numSpeakers;
            arg.endStructure();
            if (voiceId.isEmpty()) {
                continue;
            }
            const QString name = displayName.isEmpty() ? voiceId : displayName;
            QVariantMap entry;
            entry.insert(QStringLiteral("voice_id"), voiceId);
            entry.insert(QStringLiteral("display_name"), name);
            entry.insert(QStringLiteral("language"), language);
            entry.insert(QStringLiteral("num_speakers"), static_cast<int>(numSpeakers < 1 ? 1 : numSpeakers));
            entry.insert(QStringLiteral("label"), voiceLabel(name, language));
            out.append(entry);
        }
        arg.endArray();
        m_voices = out;
        Q_EMIT voicesChanged();
    });
}

void VoiceController::pushToTalk(const QString &conversationId)
{
    if (!m_available) {
        return;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    // Barge in: stop playback first so PTT isn't fighting the speaker.
    if (m_state == QStringLiteral("Speaking")) {
        bus.asyncCall(voiceCall(QStringLiteral("StopSpeaking")));
    }
    if (conversationId.isEmpty()) {
        bus.asyncCall(voiceCall(QStringLiteral("PushToTalk")));
    } else {
        QDBusMessage msg = voiceCall(QStringLiteral("PushToTalkInConversation"));
        msg.setArguments({conversationId});
        bus.asyncCall(msg);
    }
    // Optimistically reflect Listening so the UI responds instantly; the
    // daemon's StateChanged reconciles the truth.
    setState(QStringLiteral("Listening"));
}

void VoiceController::stopListening()
{
    if (!m_available) {
        return;
    }
    QDBusConnection::sessionBus().asyncCall(voiceCall(QStringLiteral("StopListening")));
    setState(QStringLiteral("Idle"));
}

void VoiceController::stopSpeaking()
{
    if (!m_available) {
        return;
    }
    QDBusConnection::sessionBus().asyncCall(voiceCall(QStringLiteral("StopSpeaking")));
}

void VoiceController::setEnabled(bool enabled)
{
    if (!m_available) {
        return;
    }
    QDBusMessage msg = voiceCall(QStringLiteral("SetEnabled"));
    msg.setArguments({enabled});
    QDBusConnection::sessionBus().asyncCall(msg);
    // Optimistic; the next GetEnabled / a failed call reconciles.
    if (m_enabled != enabled) {
        m_enabled = enabled;
        Q_EMIT enabledChanged(m_enabled);
    }
}

void VoiceController::setVoice(const QString &voiceId, int speaker)
{
    if (!m_available || voiceId.isEmpty()) {
        return;
    }
    QDBusMessage msg = voiceCall(QStringLiteral("SetVoice"));
    msg.setArguments({voiceId, speaker});
    QDBusConnection::sessionBus().asyncCall(msg);
    const int normalized = speaker >= 0 ? speaker : -1;
    if (m_voiceId != voiceId || m_speakerId != normalized) {
        m_voiceId = voiceId;
        m_speakerId = normalized;
        Q_EMIT voiceChanged();
    }
}

void VoiceController::sayText(const QString &text)
{
    if (!m_available || text.isEmpty()) {
        return;
    }
    QDBusMessage msg = voiceCall(QStringLiteral("SayText"));
    msg.setArguments({text});
    QDBusConnection::sessionBus().asyncCall(msg);
}

} // namespace adele
