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

    // --- ListMcpServersJson descriptor decode (mcp-servers-ui epic) ----------
    //
    // The MCP D-Bus edge is JSON: ListMcpServersJson returns a JSON *array* of
    // per-server descriptors that the KCM re-parses through the same
    // parseJsonReply the QML `list_mcp_servers` handler uses. This pins the
    // wire-contract shape the McpServersPage delegate reads — a top-level array,
    // a stdio descriptor, and an http+oauth descriptor with a nested oauth
    // summary — so a drift in either side is caught by the unit gate.

    void parsesMcpServerDescriptorArray()
    {
        const QString json = QStringLiteral(
            "[{\"name\":\"weather\",\"command\":\"/usr/bin/weather-mcp\","
            "\"args\":[\"--port\",\"8080\"],\"namespace\":\"wx\",\"enabled\":true,"
            "\"status\":\"running\",\"tool_count\":12,\"transport\":\"stdio\","
            "\"target\":\"/usr/bin/weather-mcp\"},"
            "{\"name\":\"gmail-work\",\"command\":\"\",\"args\":[],\"enabled\":true,"
            "\"status\":\"needs_auth\",\"tool_count\":0,\"transport\":\"http\","
            "\"target\":\"https://example.com/mcp\","
            "\"configure_label\":\"Sign in\","
            "\"configure_command\":[\"/opt/desktop-assistant\",\"--mcp-oauth-login\",\"gmail-work\"],"
            "\"auth_kind\":\"oauth\",\"oauth_authorized\":false,"
            "\"oauth_account\":\"dave@x.tech\","
            "\"oauth_scopes\":[\"https://www.googleapis.com/auth/gmail.modify\"]}]");
        const auto r = daemonreply::parseJsonReply({json});
        QVERIFY(r.ok);
        const QVariantList servers = r.value.toList();
        QCOMPARE(servers.size(), 2);

        const QVariantMap stdioSrv = servers.at(0).toMap();
        QCOMPARE(stdioSrv.value(QStringLiteral("name")).toString(), QStringLiteral("weather"));
        QCOMPARE(stdioSrv.value(QStringLiteral("transport")).toString(), QStringLiteral("stdio"));
        QCOMPARE(stdioSrv.value(QStringLiteral("status")).toString(), QStringLiteral("running"));
        QCOMPARE(stdioSrv.value(QStringLiteral("tool_count")).toInt(), 12);
        const QVariantList args = stdioSrv.value(QStringLiteral("args")).toList();
        QCOMPARE(args.size(), 2);
        QCOMPARE(args.at(1).toString(), QStringLiteral("8080"));
        // A stdio descriptor carries no configure action.
        QVERIFY(stdioSrv.value(QStringLiteral("configure_label")).toString().isEmpty());

        const QVariantMap oauthSrv = servers.at(1).toMap();
        QCOMPARE(oauthSrv.value(QStringLiteral("transport")).toString(), QStringLiteral("http"));
        QCOMPARE(oauthSrv.value(QStringLiteral("status")).toString(), QStringLiteral("needs_auth"));
        QCOMPARE(oauthSrv.value(QStringLiteral("auth_kind")).toString(), QStringLiteral("oauth"));
        QCOMPARE(oauthSrv.value(QStringLiteral("oauth_authorized")).toBool(), false);
        QCOMPARE(oauthSrv.value(QStringLiteral("oauth_account")).toString(),
                 QStringLiteral("dave@x.tech"));
        // The Sign-in button reads configure_label + spawns configure_command argv.
        QCOMPARE(oauthSrv.value(QStringLiteral("configure_label")).toString(),
                 QStringLiteral("Sign in"));
        const QVariantList configureCmd =
            oauthSrv.value(QStringLiteral("configure_command")).toList();
        QCOMPARE(configureCmd.size(), 3);
        QCOMPARE(configureCmd.at(1).toString(), QStringLiteral("--mcp-oauth-login"));
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

    // --- parsePersonalityConfig (KDE-8 / #63) --------------------------------
    //
    // The base signature here mirrors the real ConfigData leading fields
    // (sssbsssbbbbssbddui = 18 fields). Each test builds a flattened reply with
    // the matching concrete QVariant types, then appends seven u32 traits.

private:
    // A well-typed 18-field base matching "sssbsssbbbbssbddui".
    static QVariantList validBase()
    {
        return QVariantList{
            QStringLiteral("connector"), QStringLiteral("model"),
            QStringLiteral("base_url"), false,                 // s s s b
            QStringLiteral("e_conn"), QStringLiteral("e_model"),
            QStringLiteral("e_base"), false, true, false,      // s s s b b b
            true, QStringLiteral("remote"), QStringLiteral("origin"),
            false,                                             // b s s b
            double(0.7), double(0.9),                          // d d
            uint(4096), int(-1),                               // u i
        };
    }
    static QVariantList sevenTraits(uint a, uint b, uint c, uint d, uint e,
                                    uint f, uint g)
    {
        return QVariantList{a, b, c, d, e, f, g};
    }

private Q_SLOTS:
    void personalityValidatesAndParses()
    {
        QVariantList args = validBase();
        args += sevenTraits(4, 3, 3, 2, 2, 1, 1);
        const auto r = daemonreply::parsePersonalityConfig(
            args, QStringLiteral("sssbsssbbbbssbddui"), 7);
        QVERIFY2(r.signatureOk, qPrintable(r.error));
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.professionalism, 4);
        QCOMPARE(r.warmth, 3);
        QCOMPARE(r.directness, 3);
        QCOMPARE(r.enthusiasm, 2);
        QCOMPARE(r.humor, 2);
        QCOMPARE(r.sarcasm, 1);
        QCOMPARE(r.pretentiousness, 1);
    }

    void personalityClampsOutOfRangeTraits()
    {
        QVariantList args = validBase();
        args += sevenTraits(99, 0, 4, 5, 1, 2, 3); // 99 and 5 clamp to 4
        const auto r = daemonreply::parsePersonalityConfig(
            args, QStringLiteral("sssbsssbbbbssbddui"), 7);
        QVERIFY(r.signatureOk);
        QCOMPARE(r.professionalism, 4);
        QCOMPARE(r.enthusiasm, 4);
    }

    void personalityRejectsTooFewArgs()
    {
        // A field inserted/removed before the block shortens the reply (or it's
        // a pre-block daemon length+1, etc.). Anything but exactly base+7 fails.
        QVariantList args = validBase();
        args += sevenTraits(1, 1, 1, 1, 1, 1, 1);
        args.removeLast(); // base+6
        const auto r = daemonreply::parsePersonalityConfig(
            args, QStringLiteral("sssbsssbbbbssbddui"), 7);
        QVERIFY(!r.signatureOk);
        QVERIFY(!r.error.isEmpty());
    }

    void personalityRejectsTooManyArgs()
    {
        QVariantList args = validBase();
        args += sevenTraits(1, 1, 1, 1, 1, 1, 1);
        args += uint(0); // a field appended AFTER the block — base+8
        const auto r = daemonreply::parsePersonalityConfig(
            args, QStringLiteral("sssbsssbbbbssbddui"), 7);
        QVERIFY(!r.signatureOk);
    }

    void personalityRejectsBaseTypeMismatch()
    {
        // Simulate a daemon that inserted an int field where we expect the first
        // string (llm_connector): the shift makes a base field the wrong type.
        QVariantList args = validBase();
        args[0] = int(42); // expected 's', got 'i'
        args += sevenTraits(1, 1, 1, 1, 1, 1, 1);
        const auto r = daemonreply::parsePersonalityConfig(
            args, QStringLiteral("sssbsssbbbbssbddui"), 7);
        QVERIFY(!r.signatureOk);
        QVERIFY2(r.error.contains(QStringLiteral("field 0")), qPrintable(r.error));
    }

    void personalityRejectsNonU32Trait()
    {
        // The trailing block must be u32s; an int (i32) there means the block
        // shape diverged (e.g. a signed field crept in).
        QVariantList args = validBase();
        args += sevenTraits(1, 1, 1, 1, 1, 1, 1);
        args[18] = int(1); // first trait as i32, not u32
        const auto r = daemonreply::parsePersonalityConfig(
            args, QStringLiteral("sssbsssbbbbssbddui"), 7);
        QVERIFY(!r.signatureOk);
        QVERIFY2(r.error.contains(QStringLiteral("trait field 0")), qPrintable(r.error));
    }

    // --- parsePersistenceReply (KDE-2 PR 2/5) --------------------------------

    void persistenceParsesFullReply()
    {
        const QList<QVariant> args{true, QStringLiteral("git@host:repo.git"),
                                   QStringLiteral("origin"), false};
        const auto r = daemonreply::parsePersistenceReply(args);
        QVERIFY(r.ok);
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.gitEnabled, true);
        QCOMPARE(r.gitRemoteUrl, QStringLiteral("git@host:repo.git"));
        QCOMPARE(r.gitRemoteName, QStringLiteral("origin"));
        QCOMPARE(r.gitPushOnUpdate, false);
    }

    void persistenceShortReplyErrors()
    {
        const QList<QVariant> args{true, QStringLiteral("url")}; // only 2 of 4
        const auto r = daemonreply::parsePersistenceReply(args);
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QStringLiteral("Unexpected GetPersistenceSettings reply"));
    }

    // --- parseDatabaseReply --------------------------------------------------

    void databaseParsesFullReply()
    {
        const QList<QVariant> args{QStringLiteral("sqlite:///db"), uint(7)};
        const auto r = daemonreply::parseDatabaseReply(args);
        QVERIFY(r.ok);
        QCOMPARE(r.dbUrl, QStringLiteral("sqlite:///db"));
        QCOMPARE(r.dbMaxConnections, 7);
    }

    void databaseShortReplyErrors()
    {
        const auto r = daemonreply::parseDatabaseReply({QStringLiteral("url")});
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QStringLiteral("Unexpected GetDatabaseSettings reply"));
    }

    // --- parseBackendTasksReply ----------------------------------------------

    void backendTasksParsesWithArchive()
    {
        const QList<QVariant> args{true, QStringLiteral("conn"), QStringLiteral("model"),
                                   QStringLiteral("http://x"), true,
                                   qulonglong(3600), uint(30)};
        const auto r = daemonreply::parseBackendTasksReply(args);
        QVERIFY(r.ok);
        QCOMPARE(r.llmConnector, QStringLiteral("conn"));
        QCOMPARE(r.llmModel, QStringLiteral("model"));
        QCOMPARE(r.llmBaseUrl, QStringLiteral("http://x"));
        QCOMPARE(r.dreamingEnabled, true);
        QCOMPARE(r.dreamingIntervalSecs, 3600);
        QCOMPARE(r.archiveAfterDays, 30);
    }

    void backendTasksArchiveDefaultsZeroWhenAbsent()
    {
        // 6-element reply (pre-archive daemon): archiveAfterDays falls back to 0.
        const QList<QVariant> args{false, QString(), QString(), QString(),
                                   false, qulonglong(60)};
        const auto r = daemonreply::parseBackendTasksReply(args);
        QVERIFY(r.ok);
        QCOMPARE(r.dreamingIntervalSecs, 60);
        QCOMPARE(r.archiveAfterDays, 0);
    }

    void backendTasksShortReplyErrors()
    {
        const QList<QVariant> args{false, QString(), QString()}; // < 6
        const auto r = daemonreply::parseBackendTasksReply(args);
        QVERIFY(!r.ok);
        QCOMPARE(r.error, QStringLiteral("Unexpected GetBackendTasksSettings reply"));
    }

    // --- parseWsAuthReply (best-effort: short reply keeps defaults, no error) -

    void wsAuthParsesFullReply()
    {
        const QList<QVariant> args{QStringList{QStringLiteral("oidc")},
                                   QStringLiteral("https://issuer"),
                                   QStringLiteral("https://auth"),
                                   QStringLiteral("https://token"),
                                   QStringLiteral("client"),
                                   QStringLiteral("openid email")};
        const auto r = daemonreply::parseWsAuthReply(args);
        QVERIFY(r.ok);
        QCOMPARE(r.methods, (QStringList{QStringLiteral("oidc")}));
        QCOMPARE(r.oidcIssuer, QStringLiteral("https://issuer"));
        QCOMPARE(r.oidcScopes, QStringLiteral("openid email"));
    }

    void wsAuthEmptyScopesGetsDefault()
    {
        const QList<QVariant> args{QStringList{}, QString(), QString(),
                                   QString(), QString(), QString()};
        const auto r = daemonreply::parseWsAuthReply(args);
        QVERIFY(r.ok);
        QCOMPARE(r.oidcScopes, QStringLiteral("openid profile email"));
    }

    void wsAuthShortReplyKeepsDefaultsNoError()
    {
        const auto r = daemonreply::parseWsAuthReply({QStringList{}}); // < 6
        QVERIFY(!r.ok); // not populated; caller keeps in-memory defaults
    }

    // --- parsePersonalityReply -----------------------------------------------

    void personalityReadsTrailingBlock()
    {
        // 18 base fields (placeholders) + the 7-trait block.
        QList<QVariant> args;
        for (int i = 0; i < 18; ++i) {
            args << QStringLiteral("base");
        }
        args << uint(0) << uint(1) << uint(2) << uint(3) << uint(4)
             << uint(2) << uint(3);
        const auto r = daemonreply::parsePersonalityReply(args, 18);
        QVERIFY(r.present);
        QCOMPARE(r.professionalism, 0);
        QCOMPARE(r.warmth, 1);
        QCOMPARE(r.directness, 2);
        QCOMPARE(r.enthusiasm, 3);
        QCOMPARE(r.humor, 4);
        QCOMPARE(r.sarcasm, 2);
        QCOMPARE(r.pretentiousness, 3);
    }

    void personalityClampsOutOfRange()
    {
        QList<QVariant> args;
        for (int i = 0; i < 18; ++i) {
            args << QStringLiteral("base");
        }
        // Out-of-range values clamp to 0..4 (e.g. a shifted index landing on a
        // large/negative int must not propagate).
        args << int(-3) << uint(99) << uint(4) << uint(0) << uint(0)
             << uint(0) << uint(0);
        const auto r = daemonreply::parsePersonalityReply(args, 18);
        QVERIFY(r.present);
        QCOMPARE(r.professionalism, 0); // -3 -> 0
        QCOMPARE(r.warmth, 4);          // 99 -> 4
        QCOMPARE(r.directness, 4);
    }

    void personalityAbsentWhenReplyTooShort()
    {
        // Pre-#226 daemon: only the base fields, no personality block.
        QList<QVariant> args;
        for (int i = 0; i < 18; ++i) {
            args << QStringLiteral("base");
        }
        const auto r = daemonreply::parsePersonalityReply(args, 18);
        QVERIFY(!r.present);
    }

    // --- parseVoiceSelectionReply (KDE-2 / #57, PR 4/5) ----------------------

    void voiceSelectionParsesFlatForm()
    {
        // GetVoice -> (si): the common flattened form, two plain QVariants.
        const QList<QVariant> args{QStringLiteral("af_heart"), int(2)};
        const auto r = daemonreply::parseVoiceSelectionReply(args);
        QVERIFY(r.ok);
        QCOMPARE(r.voiceId, QStringLiteral("af_heart"));
        QCOMPARE(r.speaker, 2);
    }

    void voiceSelectionUnsetSpeakerIsNegativeOne()
    {
        const QList<QVariant> args{QStringLiteral("Joanna"), int(-1)};
        const auto r = daemonreply::parseVoiceSelectionReply(args);
        QVERIFY(r.ok);
        QCOMPARE(r.voiceId, QStringLiteral("Joanna"));
        QCOMPARE(r.speaker, -1);
    }

    void voiceSelectionExtraArgsParseLeadingPair()
    {
        // Defensive: a longer reply still reads the first (id, speaker) pair.
        const QList<QVariant> args{QStringLiteral("v"), int(0), QStringLiteral("x")};
        const auto r = daemonreply::parseVoiceSelectionReply(args);
        QVERIFY(r.ok);
        QCOMPARE(r.voiceId, QStringLiteral("v"));
        QCOMPARE(r.speaker, 0);
    }

    void voiceSelectionWrappedFormNotOk()
    {
        // The single-QDBusArgument wrapped struct form (one arg) is not parseable
        // flat: ok == false so the caller falls back to live demarshalling.
        const QList<QVariant> args{QStringLiteral("wrapped-struct-placeholder")};
        const auto r = daemonreply::parseVoiceSelectionReply(args);
        QVERIFY(!r.ok);
        QCOMPARE(r.speaker, -1); // default preserved
    }

    void voiceSelectionEmptyNotOk()
    {
        const auto r = daemonreply::parseVoiceSelectionReply({});
        QVERIFY(!r.ok);
    }

    // --- autostartStateToTriState (KDE-2 / #57, PR 5/5) ----------------------

    void autostartEnabledIsOne()
    {
        QCOMPARE(daemonreply::autostartStateToTriState(QStringLiteral("enabled")), 1);
        QCOMPARE(daemonreply::autostartStateToTriState(QStringLiteral("enabled-runtime")), 1);
    }

    void autostartOffStatesAreZero()
    {
        QCOMPARE(daemonreply::autostartStateToTriState(QStringLiteral("disabled")), 0);
        QCOMPARE(daemonreply::autostartStateToTriState(QStringLiteral("masked")), 0);
        QCOMPARE(daemonreply::autostartStateToTriState(QStringLiteral("masked-runtime")), 0);
        QCOMPARE(daemonreply::autostartStateToTriState(QStringLiteral("static")), 0);
    }

    void autostartUnknownIsMinusOne()
    {
        QCOMPARE(daemonreply::autostartStateToTriState(QStringLiteral("not-found")), -1);
        QCOMPARE(daemonreply::autostartStateToTriState(QString()), -1);
        QCOMPARE(daemonreply::autostartStateToTriState(QStringLiteral("garbage")), -1);
    }

    // --- peakLevelFromS16le (KDE-2 / #57, PR 5/5) ----------------------------

    void peakLevelEmptyBufferIsMinusOne()
    {
        QCOMPARE(daemonreply::peakLevelFromS16le(QByteArray()), -1.0);
        // A single byte is fewer than one whole s16 sample -> -1.
        QCOMPARE(daemonreply::peakLevelFromS16le(QByteArray(1, '\x00')), -1.0);
    }

    void peakLevelSilenceIsZero()
    {
        QByteArray pcm(8, '\x00'); // four silent samples
        QCOMPARE(daemonreply::peakLevelFromS16le(pcm), 0.0);
    }

    void peakLevelFullScaleIsOne()
    {
        // 0x7FFF == 32767 little-endian.
        QByteArray pcm;
        pcm.append('\xFF');
        pcm.append('\x7F');
        QCOMPARE(daemonreply::peakLevelFromS16le(pcm), 1.0);
    }

    void peakLevelTakesMaxMagnitudeAcrossSamples()
    {
        // Samples: +100 (0x0064), then -16384 (0xC000). Peak magnitude 16384.
        QByteArray pcm;
        pcm.append('\x64');
        pcm.append('\x00');
        pcm.append('\x00');
        pcm.append('\xC0');
        const double level = daemonreply::peakLevelFromS16le(pcm);
        QVERIFY(level > 0.49 && level < 0.51); // 16384 / 32767 ~= 0.5
    }
};

QTEST_MAIN(TestDaemonReply)
#include "tst_daemonreply.moc"
