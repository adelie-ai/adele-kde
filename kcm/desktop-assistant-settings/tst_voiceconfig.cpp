// C++ unit tests for the bus-free, file-free voice-config TOML helpers
// (voiceconfig.cpp). These cover the read-side inline-comment stripping and the
// write-side surgical merge that preserves inline comments + unknown keys
// (KDE-6 / #61), without touching the filesystem or a session bus.

#include "voiceconfig.h"

#include <QObject>
#include <QStringList>
#include <QTest>
#include <QVector>

using voiceconfig::mergeTomlLines;
using voiceconfig::stripInlineComment;
using voiceconfig::Target;

namespace {
// Convenience: a target with the common shape used throughout these tests.
Target t(const char *section, const char *key, const QString &value,
         bool quoted = false, bool omitWhenEmpty = false)
{
    return Target{QString::fromUtf8(section), QString::fromUtf8(key), value, quoted, omitWhenEmpty};
}
} // namespace

class TestVoiceConfig : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // --- stripInlineComment (KDE-6 read side) --------------------------------

    void stripNoComment()
    {
        QCOMPARE(stripInlineComment(QStringLiteral("0.45")), QStringLiteral("0.45"));
    }

    void stripTrailingComment()
    {
        QCOMPARE(stripInlineComment(QStringLiteral("0.45  # tuned by hand")),
                 QStringLiteral("0.45"));
    }

    void stripCommentNoSpaceBeforeHash()
    {
        QCOMPARE(stripInlineComment(QStringLiteral("3000# ms")), QStringLiteral("3000"));
    }

    void preservesHashInsideQuotes()
    {
        // A `#` inside a quoted string is part of the value, not a comment.
        QCOMPARE(stripInlineComment(QStringLiteral("\"pw#1\"")), QStringLiteral("\"pw#1\""));
    }

    void stripsCommentAfterQuotedValue()
    {
        QCOMPARE(stripInlineComment(QStringLiteral("\"device a\"  # the good mic")),
                 QStringLiteral("\"device a\""));
    }

    void preservesHashInsideQuotesWithSpaces()
    {
        QCOMPARE(stripInlineComment(QStringLiteral("\"a # b\"")), QStringLiteral("\"a # b\""));
    }

    // --- mergeTomlLines: comment preservation (KDE-6 write side) -------------

    void replacePreservesInlineComment()
    {
        const QStringList in{
            QStringLiteral("[wake_word]"),
            QStringLiteral("sensitivity = 0.3  # hand-tuned, do not touch"),
        };
        const QVector<Target> targets{t("wake_word", "sensitivity", QStringLiteral("0.45"))};
        const QStringList out = mergeTomlLines(in, targets);
        QCOMPARE(out.size(), 2);
        QCOMPARE(out.at(0), QStringLiteral("[wake_word]"));
        // Value rewritten, the inline comment (and its spacing) survives.
        QCOMPARE(out.at(1), QStringLiteral("sensitivity = 0.45  # hand-tuned, do not touch"));
    }

    void replaceWithoutCommentHasNoTrailer()
    {
        const QStringList in{
            QStringLiteral("[wake_word]"),
            QStringLiteral("sensitivity = 0.3"),
        };
        const QVector<Target> targets{t("wake_word", "sensitivity", QStringLiteral("0.45"))};
        const QStringList out = mergeTomlLines(in, targets);
        QCOMPARE(out.at(1), QStringLiteral("sensitivity = 0.45"));
    }

    void preservesCommentLinesAndUnknownKeys()
    {
        const QStringList in{
            QStringLiteral("# top-of-file note"),
            QStringLiteral("[wake_word]"),
            QStringLiteral("# explains sensitivity"),
            QStringLiteral("sensitivity = 0.3"),
            QStringLiteral("model_path = \"/custom/hey.rpw\"  # my model"),
        };
        const QVector<Target> targets{t("wake_word", "sensitivity", QStringLiteral("0.45"))};
        const QStringList out = mergeTomlLines(in, targets);
        // Everything except the one owned key is byte-for-byte preserved.
        QCOMPARE(out.at(0), QStringLiteral("# top-of-file note"));
        QCOMPARE(out.at(1), QStringLiteral("[wake_word]"));
        QCOMPARE(out.at(2), QStringLiteral("# explains sensitivity"));
        QCOMPARE(out.at(3), QStringLiteral("sensitivity = 0.45"));
        QCOMPARE(out.at(4), QStringLiteral("model_path = \"/custom/hey.rpw\"  # my model"));
    }

    void doesNotTouchCommentedOutKey()
    {
        // A commented-out assignment must not be treated as the live key.
        const QStringList in{
            QStringLiteral("[wake_word]"),
            QStringLiteral("# sensitivity = 0.9"),
            QStringLiteral("sensitivity = 0.3"),
        };
        const QVector<Target> targets{t("wake_word", "sensitivity", QStringLiteral("0.45"))};
        const QStringList out = mergeTomlLines(in, targets);
        QCOMPARE(out.at(1), QStringLiteral("# sensitivity = 0.9"));
        QCOMPARE(out.at(2), QStringLiteral("sensitivity = 0.45"));
    }

    // --- mergeTomlLines: append / section creation ---------------------------

    void appendsKeyToExistingSection()
    {
        const QStringList in{
            QStringLiteral("[wake_word]"),
            QStringLiteral("sensitivity = 0.45"),
        };
        const QVector<Target> targets{
            t("wake_word", "sensitivity", QStringLiteral("0.45")),
            t("wake_word", "eager", QStringLiteral("true")),
        };
        const QStringList out = mergeTomlLines(in, targets);
        // eager inserted right after the header.
        QVERIFY(out.contains(QStringLiteral("eager = true")));
        const int hdr = out.indexOf(QStringLiteral("[wake_word]"));
        QCOMPARE(out.at(hdr + 1), QStringLiteral("eager = true"));
    }

    void createsMissingSection()
    {
        const QStringList in{
            QStringLiteral("[wake_word]"),
            QStringLiteral("sensitivity = 0.45"),
        };
        const QVector<Target> targets{
            t("vad", "speech_threshold", QStringLiteral("0.5")),
        };
        const QStringList out = mergeTomlLines(in, targets);
        QVERIFY(out.contains(QStringLiteral("[vad]")));
        const int hdr = out.indexOf(QStringLiteral("[vad]"));
        QVERIFY(hdr >= 0);
        QCOMPARE(out.at(hdr + 1), QStringLiteral("speech_threshold = 0.5"));
    }

    void quotedTargetGetsQuotes()
    {
        const QStringList in{};
        const QVector<Target> targets{
            t("audio", "input_device", QStringLiteral("PipeWire Sound Server"), /*quoted=*/true),
        };
        const QStringList out = mergeTomlLines(in, targets);
        QVERIFY(out.contains(QStringLiteral("input_device = \"PipeWire Sound Server\"")));
    }

    // --- mergeTomlLines: omit-when-empty -------------------------------------

    void omitWhenEmptyDropsExistingLine()
    {
        const QStringList in{
            QStringLiteral("[tts]"),
            QStringLiteral("polly_region = \"us-east-1\""),
        };
        const QVector<Target> targets{
            t("tts", "polly_region", QString(), /*quoted=*/true, /*omitWhenEmpty=*/true),
        };
        const QStringList out = mergeTomlLines(in, targets);
        // The key line is gone; the section header stays.
        QVERIFY(out.contains(QStringLiteral("[tts]")));
        for (const QString &l : out) {
            QVERIFY2(!l.contains(QStringLiteral("polly_region")), qPrintable(l));
        }
    }

    void omitWhenEmptyDoesNotCreateSection()
    {
        const QStringList in{
            QStringLiteral("[wake_word]"),
            QStringLiteral("sensitivity = 0.45"),
        };
        const QVector<Target> targets{
            t("tts", "polly_region", QString(), /*quoted=*/true, /*omitWhenEmpty=*/true),
        };
        const QStringList out = mergeTomlLines(in, targets);
        // No [tts] section conjured for a dropped key.
        for (const QString &l : out) {
            QVERIFY2(l.trimmed() != QStringLiteral("[tts]"), "must not create section for dropped key");
        }
    }

    // --- mergeTomlLines: section scoping -------------------------------------

    void replacesKeyOnlyInMatchingSection()
    {
        // model_path exists in both [stt] and [tts]; a [tts] target must only
        // touch the [tts] one.
        const QStringList in{
            QStringLiteral("[stt]"),
            QStringLiteral("model_path = \"/whisper.bin\""),
            QStringLiteral("[tts]"),
            QStringLiteral("model_path = \"/old-piper.onnx\""),
        };
        const QVector<Target> targets{
            t("tts", "model_path", QStringLiteral("/new-piper.onnx"), /*quoted=*/true, /*omitWhenEmpty=*/true),
        };
        const QStringList out = mergeTomlLines(in, targets);
        QCOMPARE(out.at(1), QStringLiteral("model_path = \"/whisper.bin\""));
        QCOMPARE(out.at(3), QStringLiteral("model_path = \"/new-piper.onnx\""));
    }
};

QTEST_MAIN(TestVoiceConfig)
#include "tst_voiceconfig.moc"
