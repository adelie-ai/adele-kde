// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Spec for the length AdeleCore hands the length-carrying (`_n`) C ABI entry
// points (#136). Every call site builds a QByteArray with `.toUtf8()` one line
// above the FFI call and must pass that QByteArray's byte count, not the
// QString's character count. The two counts agree for ASCII text, so an ASCII
// fixture cannot catch a call site that regresses to the character count; this
// file exists to catch exactly that regression.
//
// It links its own stand-in definitions of the `adele_core_*` C ABI (below),
// not the real Rust cdylib. Each stand-in copies the bytes it was told to read
// (`ptr`, `len`) into a QByteArray. If a call site passed the QString's
// character count instead of the QByteArray's byte count, that copy would stop
// short (for text with multi-byte UTF-8 characters, the character count is
// always less than or equal to the byte count) and the captured bytes would
// no longer match the full `.toUtf8()` encoding the test compares against.
// This test never builds or links the real Rust core, so it needs no cargo
// toolchain and runs independent of client-build's cdylib step.

#include <QByteArray>
#include <QString>
#include <QTest>

#include "adelecore.h"
#include "adele_client_core.h"

using adele::AdeleCore;

namespace {

// A fixture where the UTF-8 byte count is well past the character count: nine
// QChars, sixteen UTF-8 bytes ('e'-acute is two bytes; each CJK character is
// three). A wrong (character-counted) length truncates mid-character, so the
// captured bytes cannot coincidentally match the full encoding.
const QString kNonAscii = QString::fromUtf8("h\xc3\xa9llo-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");

QByteArray copyBytes(const char *ptr, uintptr_t len)
{
    if (!ptr) {
        return QByteArray();
    }
    return QByteArray(ptr, static_cast<qsizetype>(len));
}

// One call's captured arguments. Only the fields the active test cares about
// are populated; the rest stay default.
struct Captured {
    bool called = false;
    QByteArray a;
    uintptr_t aLen = 0;
    QByteArray b;
    uintptr_t bLen = 0;
    QByteArray c;
    uintptr_t cLen = 0;
    bool boolArg = false;
};

Captured g_captured;

void reset()
{
    g_captured = Captured();
}

} // namespace

// --- Stand-in C ABI ------------------------------------------------------
// Definitions for exactly the symbols client/adelecore.cpp calls. None of the
// nine migrated entry points below do anything but capture their arguments;
// the rest are no-ops that keep the handle alive.
extern "C" {

struct Core {
};

Core *adele_core_new(void (*)(void *, const char *), void *)
{
    return new Core();
}

void adele_core_free(Core *core)
{
    delete core;
}

void adele_core_set_share_client_context(Core *, bool)
{
}

void adele_core_edit_queued(Core *, uintptr_t)
{
}

void adele_core_remove_queued(Core *, uintptr_t)
{
}

void adele_core_cancel_queued_edit(Core *)
{
}

void adele_core_new_conversation(Core *)
{
}

void adele_core_connect_n(Core *, const char *transport, uintptr_t transport_len, const char *address, uintptr_t address_len)
{
    g_captured.called = true;
    g_captured.a = copyBytes(transport, transport_len);
    g_captured.aLen = transport_len;
    g_captured.b = copyBytes(address, address_len);
    g_captured.bLen = address_len;
}

void adele_core_send_prompt_n(Core *, const char *text, uintptr_t text_len)
{
    g_captured.called = true;
    g_captured.a = copyBytes(text, text_len);
    g_captured.aLen = text_len;
}

void adele_core_select_conversation_n(Core *, const char *conversation_id, uintptr_t conversation_id_len)
{
    g_captured.called = true;
    g_captured.a = copyBytes(conversation_id, conversation_id_len);
    g_captured.aLen = conversation_id_len;
}

void adele_core_delete_conversation_n(Core *, const char *conversation_id, uintptr_t conversation_id_len)
{
    g_captured.called = true;
    g_captured.a = copyBytes(conversation_id, conversation_id_len);
    g_captured.aLen = conversation_id_len;
}

void adele_core_set_voice_in_n(Core *, const char *conversation_id, uintptr_t conversation_id_len, bool enabled)
{
    g_captured.called = true;
    g_captured.a = copyBytes(conversation_id, conversation_id_len);
    g_captured.aLen = conversation_id_len;
    g_captured.boolArg = enabled;
}

void adele_core_set_adele_output_n(Core *,
                                   const char *conversation_id,
                                   uintptr_t conversation_id_len,
                                   const char *level,
                                   uintptr_t level_len)
{
    g_captured.called = true;
    g_captured.a = copyBytes(conversation_id, conversation_id_len);
    g_captured.aLen = conversation_id_len;
    g_captured.b = copyBytes(level, level_len);
    g_captured.bLen = level_len;
}

void adele_core_select_model_n(Core *,
                               const char *connection_id,
                               uintptr_t connection_id_len,
                               const char *model_id,
                               uintptr_t model_id_len,
                               const char *effort,
                               uintptr_t effort_len)
{
    g_captured.called = true;
    g_captured.a = copyBytes(connection_id, connection_id_len);
    g_captured.aLen = connection_id_len;
    g_captured.b = copyBytes(model_id, model_id_len);
    g_captured.bLen = model_id_len;
    g_captured.c = copyBytes(effort, effort_len);
    g_captured.cLen = effort_len;
}

void adele_core_cancel_task_n(Core *, const char *task_id, uintptr_t task_id_len)
{
    g_captured.called = true;
    g_captured.a = copyBytes(task_id, task_id_len);
    g_captured.aLen = task_id_len;
}

void adele_core_fetch_task_logs_n(Core *, const char *task_id, uintptr_t task_id_len)
{
    g_captured.called = true;
    g_captured.a = copyBytes(task_id, task_id_len);
    g_captured.aLen = task_id_len;
}

} // extern "C"

// --- Tests -----------------------------------------------------------------

class TestAdeleCoreFfiLengths : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();

    void connectPassesUtf8ByteLengthsNotCharacterCounts();
    void sendPromptPassesUtf8ByteLength();
    void selectConversationPassesUtf8ByteLength();
    void deleteConversationPassesUtf8ByteLength();
    void setVoiceInPassesUtf8ByteLength();
    void setAdeleOutputPassesUtf8ByteLengths();
    void selectModelPassesUtf8ByteLengths();
    void cancelTaskPassesUtf8ByteLength();
    void fetchTaskLogsPassesUtf8ByteLength();
};

void TestAdeleCoreFfiLengths::init()
{
    reset();
    // The fixture's character count must differ from its byte count, or this
    // whole file would pass even with a character-counted regression.
    QVERIFY(kNonAscii.size() != kNonAscii.toUtf8().size());
}

void TestAdeleCoreFfiLengths::connectPassesUtf8ByteLengthsNotCharacterCounts()
{
    AdeleCore core;
    core.connectToDaemon(kNonAscii, kNonAscii);
    QVERIFY(g_captured.called);
    const QByteArray expected = kNonAscii.toUtf8();
    QCOMPARE(g_captured.aLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.a, expected);
    QCOMPARE(g_captured.bLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.b, expected);
}

void TestAdeleCoreFfiLengths::sendPromptPassesUtf8ByteLength()
{
    AdeleCore core;
    core.sendPrompt(kNonAscii);
    QVERIFY(g_captured.called);
    const QByteArray expected = kNonAscii.toUtf8();
    QCOMPARE(g_captured.aLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.a, expected);
}

void TestAdeleCoreFfiLengths::selectConversationPassesUtf8ByteLength()
{
    AdeleCore core;
    core.selectConversation(kNonAscii);
    QVERIFY(g_captured.called);
    const QByteArray expected = kNonAscii.toUtf8();
    QCOMPARE(g_captured.aLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.a, expected);
}

void TestAdeleCoreFfiLengths::deleteConversationPassesUtf8ByteLength()
{
    AdeleCore core;
    core.deleteConversation(kNonAscii);
    QVERIFY(g_captured.called);
    const QByteArray expected = kNonAscii.toUtf8();
    QCOMPARE(g_captured.aLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.a, expected);
}

void TestAdeleCoreFfiLengths::setVoiceInPassesUtf8ByteLength()
{
    AdeleCore core;
    core.setVoiceIn(kNonAscii, true);
    QVERIFY(g_captured.called);
    const QByteArray expected = kNonAscii.toUtf8();
    QCOMPARE(g_captured.aLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.a, expected);
    QVERIFY(g_captured.boolArg);
}

void TestAdeleCoreFfiLengths::setAdeleOutputPassesUtf8ByteLengths()
{
    AdeleCore core;
    core.setAdeleOutput(kNonAscii, kNonAscii);
    QVERIFY(g_captured.called);
    const QByteArray expected = kNonAscii.toUtf8();
    QCOMPARE(g_captured.aLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.a, expected);
    QCOMPARE(g_captured.bLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.b, expected);
}

void TestAdeleCoreFfiLengths::selectModelPassesUtf8ByteLengths()
{
    AdeleCore core;
    core.selectModel(kNonAscii, kNonAscii, kNonAscii);
    QVERIFY(g_captured.called);
    const QByteArray expected = kNonAscii.toUtf8();
    QCOMPARE(g_captured.aLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.a, expected);
    QCOMPARE(g_captured.bLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.b, expected);
    QCOMPARE(g_captured.cLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.c, expected);
}

void TestAdeleCoreFfiLengths::cancelTaskPassesUtf8ByteLength()
{
    AdeleCore core;
    core.cancelTask(kNonAscii);
    QVERIFY(g_captured.called);
    const QByteArray expected = kNonAscii.toUtf8();
    QCOMPARE(g_captured.aLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.a, expected);
}

void TestAdeleCoreFfiLengths::fetchTaskLogsPassesUtf8ByteLength()
{
    AdeleCore core;
    core.fetchTaskLogs(kNonAscii);
    QVERIFY(g_captured.called);
    const QByteArray expected = kNonAscii.toUtf8();
    QCOMPARE(g_captured.aLen, static_cast<uintptr_t>(expected.size()));
    QCOMPARE(g_captured.a, expected);
}

QTEST_MAIN(TestAdeleCoreFfiLengths)
#include "tst_adelecore_ffi_lengths.moc"
