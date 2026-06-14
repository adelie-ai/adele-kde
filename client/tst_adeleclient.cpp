// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Spec for AdeleDaemon's bus-free behaviour: graceful connect-failure and the
// tagged-JSON -> Qt-signal marshalling (the core of the live-render path). The
// live UDS path itself needs a running daemon and is covered by manual QA + the
// daemon's own tests; here we exercise everything that needs no daemon.

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include "adeleclient.h"

using adele::AdeleDaemon;

class TestAdeleDaemon : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void connectToMissingDaemonDegradesToDisconnected();
    void subscribeWhileDisconnectedReturnsFalse();
    void sendPromptWhileDisconnectedReturnsEmpty();

    void userMessageAddedCarriesAllFields();
    void chunkIsEmitted();
    void completeMapsToCompleted();
    void errorIsEmitted();
    void statusIsEmitted();
    void titleChangedIsEmitted();
    void conversationListChangedIsEmitted();
    void clientToolCallPreservesArgumentsAsJson();
    void disconnectedIsEmitted();

    void malformedJsonEmitsNothing();
    void unknownKindEmitsNothing();
    void missingFieldsYieldEmptyStrings();

private:
    // Drive the private Q_INVOKABLE dispatchEvent synchronously (same thread).
    static void dispatch(AdeleDaemon &d, const QString &json)
    {
        QVERIFY(QMetaObject::invokeMethod(&d, "dispatchEvent", Qt::DirectConnection, Q_ARG(QString, json)));
    }
};

// --- Connection-state / graceful degradation ---------------------------------

void TestAdeleDaemon::connectToMissingDaemonDegradesToDisconnected()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::connectedChanged);

    // Bogus paths => the FFI's connect fails fast (ENOENT) and returns null.
    const bool ok = daemon.connectToDaemon(QStringLiteral("/nonexistent/adele-test-sock"),
                                            QStringLiteral("/nonexistent/adele-test-mint"));

    QVERIFY(!ok);
    QVERIFY(!daemon.isConnected());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), false);
}

void TestAdeleDaemon::subscribeWhileDisconnectedReturnsFalse()
{
    AdeleDaemon daemon;
    QVERIFY(!daemon.subscribeConversations({QStringLiteral("c1"), QStringLiteral("c2")}));
}

void TestAdeleDaemon::sendPromptWhileDisconnectedReturnsEmpty()
{
    AdeleDaemon daemon;
    QVERIFY(daemon.sendPrompt(QStringLiteral("c1"), QStringLiteral("hello")).isEmpty());
}

// --- tagged-JSON -> signal marshalling ---------------------------------------

void TestAdeleDaemon::userMessageAddedCarriesAllFields()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::userMessageAdded);
    dispatch(daemon,
             QStringLiteral(R"({"kind":"user_message_added","conversation_id":"c","request_id":"r","content":"hi"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("c"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("r"));
    QCOMPARE(spy.at(0).at(2).toString(), QStringLiteral("hi"));
}

void TestAdeleDaemon::chunkIsEmitted()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::chunkReceived);
    dispatch(daemon, QStringLiteral(R"({"kind":"chunk","conversation_id":"c","request_id":"r","chunk":"to"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(2).toString(), QStringLiteral("to"));
}

void TestAdeleDaemon::completeMapsToCompleted()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::completed);
    dispatch(daemon,
             QStringLiteral(R"({"kind":"complete","conversation_id":"c","request_id":"r","full_response":"done"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(2).toString(), QStringLiteral("done"));
}

void TestAdeleDaemon::errorIsEmitted()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::errorReceived);
    dispatch(daemon, QStringLiteral(R"({"kind":"error","conversation_id":"c","request_id":"r","error":"boom"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(2).toString(), QStringLiteral("boom"));
}

void TestAdeleDaemon::statusIsEmitted()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::statusReceived);
    dispatch(daemon,
             QStringLiteral(R"({"kind":"status","conversation_id":"c","request_id":"r","message":"thinking"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(2).toString(), QStringLiteral("thinking"));
}

void TestAdeleDaemon::titleChangedIsEmitted()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::titleChanged);
    dispatch(daemon, QStringLiteral(R"({"kind":"title_changed","conversation_id":"c","title":"New Title"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("c"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("New Title"));
}

void TestAdeleDaemon::conversationListChangedIsEmitted()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::conversationListChanged);
    dispatch(daemon, QStringLiteral(R"({"kind":"conversation_list_changed","conversation_id":"c"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("c"));
}

void TestAdeleDaemon::clientToolCallPreservesArgumentsAsJson()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::clientToolCall);
    dispatch(daemon,
             QStringLiteral(R"({"kind":"client_tool_call","task_id":"t","conversation_id":"c",)"
                            R"("tool_call_id":"tc","tool_name":"echo","arguments":{"x":1,"y":"z"}})"));
    QCOMPARE(spy.count(), 1);
    const auto row = spy.at(0);
    QCOMPARE(row.at(0).toString(), QStringLiteral("t"));
    QCOMPARE(row.at(3).toString(), QStringLiteral("echo"));
    // The arguments must arrive as parseable JSON, structurally intact.
    const QJsonDocument args = QJsonDocument::fromJson(row.at(4).toString().toUtf8());
    QVERIFY(args.isObject());
    QCOMPARE(args.object().value(QStringLiteral("x")).toInt(), 1);
    QCOMPARE(args.object().value(QStringLiteral("y")).toString(), QStringLiteral("z"));
}

void TestAdeleDaemon::disconnectedIsEmitted()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::disconnected);
    dispatch(daemon, QStringLiteral(R"({"kind":"disconnected","reason":"daemon restart"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("daemon restart"));
}

// --- edge cases --------------------------------------------------------------

void TestAdeleDaemon::malformedJsonEmitsNothing()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::userMessageAdded);
    dispatch(daemon, QStringLiteral("not json at all {"));
    dispatch(daemon, QStringLiteral("[1,2,3]")); // valid JSON, but not an object
    QCOMPARE(spy.count(), 0);
}

void TestAdeleDaemon::unknownKindEmitsNothing()
{
    AdeleDaemon daemon;
    QSignalSpy uma(&daemon, &AdeleDaemon::userMessageAdded);
    QSignalSpy chunk(&daemon, &AdeleDaemon::chunkReceived);
    dispatch(daemon, QStringLiteral(R"({"kind":"context_usage","conversation_id":"c","used":5})"));
    dispatch(daemon, QStringLiteral(R"({"conversation_id":"c"})")); // no "kind"
    QCOMPARE(uma.count(), 0);
    QCOMPARE(chunk.count(), 0);
}

void TestAdeleDaemon::missingFieldsYieldEmptyStrings()
{
    AdeleDaemon daemon;
    QSignalSpy spy(&daemon, &AdeleDaemon::userMessageAdded);
    // kind present, payload fields absent -> emitted with empty strings, no crash.
    dispatch(daemon, QStringLiteral(R"({"kind":"user_message_added"})"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QString());
    QCOMPARE(spy.at(0).at(2).toString(), QString());
}

QTEST_MAIN(TestAdeleDaemon)
#include "tst_adeleclient.moc"
