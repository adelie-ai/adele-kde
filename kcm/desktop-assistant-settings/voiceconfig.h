#pragma once

// Pure, file-free helpers for the voice TOML config read/write path
// (~/.config/adele-voice/config.toml).
//
// The KCM does a surgical, section-aware merge: it owns a handful of scalar
// keys under [audio]/[wake_word]/[vad]/[assistant]/[stt]/[tts] and rewrites
// ONLY those, preserving every other line (comments, unknown keys, sections it
// doesn't manage) so the user's hand-tuned config survives a settings change.
//
// These functions are deliberately free of QFile / the bus so the C++ test
// target (tst_voiceconfig) can exercise the parse/merge/comment-preservation
// behaviour on plain strings without touching the filesystem. The KCM
// (desktopassistantkcm.cpp) does the file I/O (atomic QSaveFile) and calls
// these for the actual line work.

#include <QString>
#include <QStringList>
#include <QVector>

namespace voiceconfig {

// Strip a TOML inline comment (` #...`) from a value, honouring double-quoted
// strings so a `#` inside quotes is NOT treated as a comment. Returns the value
// with any trailing comment and surrounding whitespace removed.
//
// KDE-6 (#61): the previous reader ran `toDouble`/`toInt` on everything after
// `=`, so `sensitivity = 0.45  # tuned` failed to parse and the UI silently
// reverted to a default the user never chose. Callers run this before parsing a
// scalar value.
//
// Examples:
//   `0.45  # tuned`        -> `0.45`
//   `"pw#1"  # device`     -> `"pw#1"`   (the in-quote # is preserved)
//   `"a # b"`              -> `"a # b"`
QString stripInlineComment(const QString &value);

// One key the KCM owns. `value` is the raw string to write (formatLine adds the
// quotes when `quoted`); `omitWhenEmpty` means an empty value drops the key
// entirely (and removes any existing line) so clearing a GUI field restores the
// daemon's default.
struct Target {
    QString section;
    QString key;
    QString value;
    bool quoted = false;
    bool omitWhenEmpty = false;
};

// Surgically merge `targets` into the existing config `lines`, returning the new
// line list. Behaviour:
//   * Replaces each owned key in place within its section, PRESERVING any
//     trailing inline comment on the existing line (KDE-6 / #61) — so
//     `sensitivity = 0.3  # hand-tuned` keeps `  # hand-tuned` after the value
//     is rewritten.
//   * Leaves every other line untouched (comments, blank lines, unknown keys,
//     unmanaged sections).
//   * omit-when-empty targets with a blank value are dropped: not emitted, and
//     any existing line for them is removed.
//   * Appends keys whose section exists but lacked the key (right after the
//     section header), and creates missing sections at the end.
//
// Pure: no I/O, deterministic, fully unit-testable.
QStringList mergeTomlLines(const QStringList &lines, const QVector<Target> &targets);

} // namespace voiceconfig
