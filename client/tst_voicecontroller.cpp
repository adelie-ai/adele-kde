// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Spec for VoiceController's bus-free behaviour: lifecycle, default (unavailable
// / Idle) state, the pure label helper, and that every intent is a safe no-op
// while the service is unavailable (the capability-degradation guard) — so a
// headless / daemon-down session never crashes or fires stray D-Bus traffic.
// The live D-Bus path (signals, method round-trips) needs the running voice
// daemon and is covered by manual QA + the daemon's own tests.

#include <QSignalSpy>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include "voicecontroller.h"

using adele::VoiceController;

class TestVoiceController : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void constructAndDestroyDoesNotCrash();
    void defaultsAreIdleAndUnavailable();
    void voiceLabelFormatsNameAndLanguage();
    void intentsWhileUnavailableAreNoOps();
    void startDoesNotCrashAndIsIdempotent();
};

void TestVoiceController::constructAndDestroyDoesNotCrash()
{
    VoiceController vc;
    QVERIFY(!vc.isAvailable());
}

void TestVoiceController::defaultsAreIdleAndUnavailable()
{
    VoiceController vc;
    QVERIFY(!vc.isAvailable());
    QCOMPARE(vc.state(), QStringLiteral("Idle"));
    QVERIFY(!vc.isEnabled());
    QCOMPARE(vc.muteSecondsRemaining(), 0);
    QVERIFY(vc.voiceId().isEmpty());
    QCOMPARE(vc.speakerId(), -1);
    QVERIFY(vc.voices().isEmpty());
}

void TestVoiceController::voiceLabelFormatsNameAndLanguage()
{
    QCOMPARE(VoiceController::voiceLabel(QStringLiteral("Amy"), QStringLiteral("en_US")),
             QStringLiteral("Amy (en_US)"));
    // No language ⇒ just the name (no empty parens).
    QCOMPARE(VoiceController::voiceLabel(QStringLiteral("Amy"), QString()), QStringLiteral("Amy"));
}

void TestVoiceController::intentsWhileUnavailableAreNoOps()
{
    // available == false (no start / no daemon): every intent must early-return
    // without touching the bus, mutating optimistic state, or crashing.
    VoiceController vc;
    QSignalSpy stateSpy(&vc, &VoiceController::stateChanged);
    QSignalSpy enabledSpy(&vc, &VoiceController::enabledChanged);
    QSignalSpy muteSpy(&vc, &VoiceController::muteSecondsRemainingChanged);
    QSignalSpy voiceSpy(&vc, &VoiceController::voiceChanged);

    vc.pushToTalk(QStringLiteral("c1"));
    vc.stopListening();
    vc.stopSpeaking();
    vc.setEnabled(true);
    vc.muteFor(1800);
    vc.unmute();
    vc.setVoice(QStringLiteral("amy"), 2);
    vc.sayText(QStringLiteral("hello"));
    vc.refreshVoices();

    QCOMPARE(vc.state(), QStringLiteral("Idle"));
    QVERIFY(!vc.isEnabled());
    QCOMPARE(vc.muteSecondsRemaining(), 0);
    QVERIFY(vc.voiceId().isEmpty());
    QCOMPARE(vc.speakerId(), -1);
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(enabledSpy.count(), 0);
    QCOMPARE(muteSpy.count(), 0);
    QCOMPARE(voiceSpy.count(), 0);
}

void TestVoiceController::startDoesNotCrashAndIsIdempotent()
{
    // start() may or may not find a session bus / the voice daemon — both are
    // fine. We only assert it never crashes and is safe to call twice. (We do
    // NOT assert `available`: that depends on whether the daemon is live, which
    // a unit test must not require.)
    VoiceController vc;
    vc.start();
    vc.start();
    QTest::qWait(20);
    QVERIFY(true);
}

QTEST_MAIN(TestVoiceController)
#include "tst_voicecontroller.moc"
