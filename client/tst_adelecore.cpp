// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Spec for AdeleCore's bus-free behaviour: lifecycle (create/destroy the Rust
// core), the JSON-view-event -> Qt-signal marshalling (the core of the live
// render path), the `connected` convenience property, and that user intents made
// before a connection degrade gracefully (the C++ guards + the core's
// no-connector path) rather than crashing. The live D-Bus path itself needs a
// running daemon and is covered by manual QA + the daemon's own tests.

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include "adelecore.h"

using adele::AdeleCore;

class TestAdeleCore : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void constructAndDestroyDoesNotCrash();
    void intentsBeforeConnectDoNotCrash();
    void queueIntentsBeforeConnectDoNotCrash();

    void dispatchEmitsViewEventWithType();
    void dispatchPreservesNestedData();
    void connectedPropertyTracksLifecycleEvents();

    void composerTextEventMarshalsToViewEvent();
    void queuedMessagesEventMarshalsToViewEvent();
    void queuedMessagesEventEditingNullMarshals();

    void malformedJsonEmitsNothing();
    void nonObjectJsonEmitsNothing();
    void missingTypeEmitsNothing();

private:
    // Drive the private Q_INVOKABLE dispatchEvent synchronously (same thread).
    static void dispatch(AdeleCore &c, const QString &json)
    {
        QVERIFY(QMetaObject::invokeMethod(&c, "dispatchEvent", Qt::DirectConnection, Q_ARG(QString, json)));
    }
};

// --- Lifecycle / graceful degradation ----------------------------------------

void TestAdeleCore::constructAndDestroyDoesNotCrash()
{
    // The ctor builds the Rust runtime + reducer actor; the dtor frees it.
    AdeleCore core;
    QVERIFY(!core.isConnected());
}

void TestAdeleCore::intentsBeforeConnectDoNotCrash()
{
    // Intents are fire-and-forget into the core. With no connection the core's
    // RPC paths early-return; the C++ side forwards regardless. None must crash.
    AdeleCore core;
    core.sendPrompt(QStringLiteral("hello"));
    core.selectConversation(QStringLiteral("c1"));
    core.newConversation();
    core.deleteConversation(QStringLiteral("c1"));
    core.setVoiceIn(QStringLiteral("c1"), true);
    core.setAdeleOutput(QStringLiteral("c1"), QStringLiteral("on_demand"));
    core.selectModel(QStringLiteral("conn"), QStringLiteral("model"), QStringLiteral("high"));
    core.cancelTask(QStringLiteral("t1"));
    core.fetchTaskLogs(QStringLiteral("t1"));
    core.connectToDaemon(QStringLiteral("uds"), QStringLiteral("/nonexistent/adele-test.sock"));
    // Let any queued work run; the object must remain alive and disconnected.
    QTest::qWait(50);
    QVERIFY(!core.isConnected());
}

void TestAdeleCore::queueIntentsBeforeConnectDoNotCrash()
{
    // The message-queue intents are fire-and-forget into the core. With no
    // connection they early-return in the reducer; the C++ side forwards
    // regardless. A negative index must be guarded so it never becomes a huge
    // uintptr_t, and an out-of-range positive index is a reducer no-op. None
    // must crash.
    AdeleCore core;
    core.editQueued(0);
    core.editQueued(5);
    core.editQueued(-1);
    core.removeQueued(0);
    core.removeQueued(-3);
    core.cancelQueuedEdit();
    QTest::qWait(20);
    QVERIFY(!core.isConnected());
}

// --- view-event JSON -> signal marshalling -----------------------------------

void TestAdeleCore::dispatchEmitsViewEventWithType()
{
    AdeleCore core;
    QSignalSpy spy(&core, &AdeleCore::viewEvent);
    dispatch(core, QStringLiteral(R"({"type":"chunk","text":"to"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("chunk"));
    const auto data = spy.at(0).at(1).toMap();
    QCOMPARE(data.value(QStringLiteral("text")).toString(), QStringLiteral("to"));
}

void TestAdeleCore::dispatchPreservesNestedData()
{
    AdeleCore core;
    QSignalSpy spy(&core, &AdeleCore::viewEvent);
    dispatch(core,
             QStringLiteral(R"({"type":"conversations","items":[)"
                            R"({"id":"c1","title":"First","message_count":3,"archived":false}]})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("conversations"));
    const auto data = spy.at(0).at(1).toMap();
    const QVariantList items = data.value(QStringLiteral("items")).toList();
    QCOMPARE(items.size(), 1);
    const auto first = items.at(0).toMap();
    QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("c1"));
    QCOMPARE(first.value(QStringLiteral("message_count")).toInt(), 3);
    QCOMPARE(first.value(QStringLiteral("archived")).toBool(), false);
}

void TestAdeleCore::connectedPropertyTracksLifecycleEvents()
{
    AdeleCore core;
    QSignalSpy spy(&core, &AdeleCore::connectedChanged);

    dispatch(core, QStringLiteral(R"({"type":"connected","label":"D-Bus bridge"})"));
    QVERIFY(core.isConnected());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);

    // A duplicate connected must not re-emit.
    dispatch(core, QStringLiteral(R"({"type":"connected","label":"D-Bus bridge"})"));
    QCOMPARE(spy.count(), 1);

    dispatch(core, QStringLiteral(R"({"type":"client_cleared"})"));
    QVERIFY(!core.isConnected());
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);

    // connect_error also reflects as not-connected (no spurious re-emit when
    // already disconnected).
    dispatch(core, QStringLiteral(R"({"type":"connect_error","message":"refused"})"));
    QVERIFY(!core.isConnected());
    QCOMPARE(spy.count(), 2);
}

// --- queue view-events (composer_text / queued_messages) ---------------------

void TestAdeleCore::composerTextEventMarshalsToViewEvent()
{
    // The reducer sets the live composer via a `composer_text` event (recall
    // load, or an empty string to clear on enqueue/cancel). It must reach QML
    // through the generic viewEvent marshalling with its text intact.
    AdeleCore core;
    QSignalSpy spy(&core, &AdeleCore::viewEvent);
    dispatch(core, QStringLiteral(R"({"type":"composer_text","text":"recalled draft"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("composer_text"));
    QCOMPARE(spy.at(0).at(1).toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("recalled draft"));
}

void TestAdeleCore::queuedMessagesEventMarshalsToViewEvent()
{
    // The queue snapshot carries the messages (submit order) and the index
    // currently checked out for editing. Both must survive to QML.
    AdeleCore core;
    QSignalSpy spy(&core, &AdeleCore::viewEvent);
    dispatch(core,
             QStringLiteral(R"({"type":"queued_messages","messages":["first","second"],"editing":1})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("queued_messages"));
    const auto data = spy.at(0).at(1).toMap();
    const QVariantList messages = data.value(QStringLiteral("messages")).toList();
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.at(0).toString(), QStringLiteral("first"));
    QCOMPARE(messages.at(1).toString(), QStringLiteral("second"));
    QCOMPARE(data.value(QStringLiteral("editing")).toInt(), 1);
}

void TestAdeleCore::queuedMessagesEventEditingNullMarshals()
{
    // A JSON null `editing` must survive as a null QVariant (not coerced to an
    // integer), so the QML side can tell "not editing" apart from index 0.
    AdeleCore core;
    QSignalSpy spy(&core, &AdeleCore::viewEvent);
    dispatch(core, QStringLiteral(R"({"type":"queued_messages","messages":[],"editing":null})"));
    QCOMPARE(spy.count(), 1);
    const auto data = spy.at(0).at(1).toMap();
    QCOMPARE(data.value(QStringLiteral("messages")).toList().size(), 0);
    QVERIFY(data.value(QStringLiteral("editing")).isNull());
}

// --- edge cases --------------------------------------------------------------

void TestAdeleCore::malformedJsonEmitsNothing()
{
    AdeleCore core;
    QSignalSpy spy(&core, &AdeleCore::viewEvent);
    dispatch(core, QStringLiteral("not json at all {"));
    QCOMPARE(spy.count(), 0);
}

void TestAdeleCore::nonObjectJsonEmitsNothing()
{
    AdeleCore core;
    QSignalSpy spy(&core, &AdeleCore::viewEvent);
    dispatch(core, QStringLiteral("[1,2,3]")); // valid JSON, but not an object
    QCOMPARE(spy.count(), 0);
}

void TestAdeleCore::missingTypeEmitsNothing()
{
    AdeleCore core;
    QSignalSpy spy(&core, &AdeleCore::viewEvent);
    dispatch(core, QStringLiteral(R"({"text":"orphan"})")); // no "type" tag
    QCOMPARE(spy.count(), 0);
}

QTEST_MAIN(TestAdeleCore)
#include "tst_adelecore.moc"
