// C++ unit tests for the bus-free daemon-reply parsing helpers (KDE-2 / #57).
//
// This is adele-kde's first C++ QtTest target. The async D-Bus call path in the
// KCM needs a live session bus and a System Settings host to exercise directly,
// so the design (issue #57, point 6) factors the reply-parsing logic into free
// functions that take a plain `QList<QVariant>` — exactly what these tests pass
// in — so the parse/error behaviour is covered without a bus.

#include "daemonreply.h"

#include <QObject>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

class TestDaemonReply : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // --- parseJsonReply ------------------------------------------------------

    void parsesObjectPayload()
    {
        const QList<QVariant> args{QStringLiteral("{\"ok\":true,\"id\":\"abc\"}")};
        const auto r = daemonreply::parseJsonReply(args);
        QVERIFY(r.ok);
        QVERIFY(r.error.isEmpty());
        const QVariantMap map = r.value.toMap();
        QCOMPARE(map.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(map.value(QStringLiteral("id")).toString(), QStringLiteral("abc"));
    }

    void parsesArrayPayload()
    {
        const QList<QVariant> args{QStringLiteral("[1,2,3]")};
        const auto r = daemonreply::parseJsonReply(args);
        QVERIFY(r.ok);
        const QVariantList list = r.value.toList();
        QCOMPARE(list.size(), 3);
        QCOMPARE(list.at(1).toInt(), 2);
    }

    void emptyArgsReportsMissingPayload()
    {
        const auto r = daemonreply::parseJsonReply({});
        QVERIFY(!r.ok);
        QVERIFY(r.value.isNull());
        QCOMPARE(r.error, QStringLiteral("D-Bus reply missing JSON payload"));
    }

    void malformedJsonReportsParseError()
    {
        const QList<QVariant> args{QStringLiteral("{not valid json")};
        const auto r = daemonreply::parseJsonReply(args);
        QVERIFY(!r.ok);
        QVERIFY(r.value.isNull());
        QVERIFY2(r.error.startsWith(QStringLiteral("Failed to parse daemon reply:")),
                 qPrintable(r.error));
    }

    void emptyStringIsParseError()
    {
        // An empty string is present (args not empty) but is not valid JSON.
        const QList<QVariant> args{QString()};
        const auto r = daemonreply::parseJsonReply(args);
        QVERIFY(!r.ok);
        QVERIFY2(r.error.startsWith(QStringLiteral("Failed to parse daemon reply:")),
                 qPrintable(r.error));
    }

    void ignoresExtraArguments()
    {
        // Only the first argument carries the JSON payload; trailing args are
        // ignored, matching the historical inline behaviour.
        const QList<QVariant> args{QStringLiteral("{\"v\":1}"), QStringLiteral("ignored")};
        const auto r = daemonreply::parseJsonReply(args);
        QVERIFY(r.ok);
        QCOMPARE(r.value.toMap().value(QStringLiteral("v")).toInt(), 1);
    }

    // --- dbusErrorMessage ----------------------------------------------------

    void errorMessagePreferred()
    {
        QCOMPARE(daemonreply::dbusErrorMessage(QStringLiteral("org.fd.Error.X"),
                                               QStringLiteral("boom")),
                 QStringLiteral("boom"));
    }

    void errorNameUsedWhenMessageEmpty()
    {
        QCOMPARE(daemonreply::dbusErrorMessage(QStringLiteral("org.fd.Error.NoReply"),
                                               QString()),
                 QStringLiteral("org.fd.Error.NoReply"));
    }

    void genericFallbackWhenBothEmpty()
    {
        QCOMPARE(daemonreply::dbusErrorMessage(QString(), QString()),
                 QStringLiteral("D-Bus call failed"));
    }
};

QTEST_MAIN(TestDaemonReply)
#include "tst_daemonreply.moc"
