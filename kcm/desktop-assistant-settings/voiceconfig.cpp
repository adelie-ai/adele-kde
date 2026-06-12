#include "voiceconfig.h"

namespace voiceconfig {

namespace {

// Index of the start of a TOML inline comment in `s` (the ` #` that begins a
// trailing comment), or -1 if there is none. A `#` inside a double-quoted
// string does not start a comment. TOML basic strings honour `\"` escapes, so
// we track them too.
int inlineCommentStart(const QString &s)
{
    bool inString = false;
    bool escaped = false;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == QLatin1Char('\\')) {
                escaped = true;
            } else if (c == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            inString = true;
        } else if (c == QLatin1Char('#')) {
            return i;
        }
    }
    return -1;
}

} // namespace

QString stripInlineComment(const QString &value)
{
    const int hash = inlineCommentStart(value);
    if (hash < 0) {
        return value.trimmed();
    }
    return value.left(hash).trimmed();
}

namespace {

// Build the `key = value` (or `key = "value"`) text for a target, then re-attach
// `comment` (the full `# ...` tail, already including its leading whitespace) so
// a hand-written inline comment survives the rewrite (KDE-6 / #61).
QString formatLine(const Target &t, const QString &comment)
{
    const QString rhs = t.quoted ? (QLatin1Char('"') + t.value + QLatin1Char('"')) : t.value;
    QString line = t.key + QStringLiteral(" = ") + rhs;
    if (!comment.isEmpty()) {
        // `comment` already carries the leading separator whitespace captured
        // from the original line; emit it verbatim so spacing round-trips.
        line += comment;
    }
    return line;
}

bool shouldEmit(const Target &t)
{
    return !(t.omitWhenEmpty && t.value.isEmpty());
}

// Extract the trailing inline comment (`  # ...`, including its leading
// whitespace) from a raw config line, or an empty string if there is none. The
// returned text starts at the run of whitespace that precedes the `#` so it can
// be re-appended verbatim after a rewritten value.
QString trailingComment(const QString &raw)
{
    const int hash = inlineCommentStart(raw);
    if (hash < 0) {
        return QString();
    }
    int start = hash;
    while (start > 0 && raw.at(start - 1).isSpace()) {
        --start;
    }
    return raw.mid(start);
}

} // namespace

QStringList mergeTomlLines(const QStringList &lines, const QVector<Target> &inputTargets)
{
    QVector<Target> targets = inputTargets;
    QVector<bool> done(targets.size(), false);

    // First pass: update existing keys in place within their section, preserving
    // each line's trailing inline comment. A dropped (omit-when-empty, blank)
    // target is marked done and its line removed.
    QString currentSection;
    QStringList merged;
    for (const QString &raw : lines) {
        const QString trimmed = raw.trimmed();
        if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
            currentSection = trimmed.mid(1, trimmed.size() - 2).trimmed();
            merged.push_back(raw);
            continue;
        }
        bool replaced = false;
        const int eq = trimmed.indexOf(QLatin1Char('='));
        if (eq > 0 && !trimmed.startsWith(QLatin1Char('#'))) {
            const QString key = trimmed.left(eq).trimmed();
            for (int i = 0; i < targets.size(); ++i) {
                Target &t = targets[i];
                if (!done[i] && currentSection == t.section && key == t.key) {
                    if (shouldEmit(t)) {
                        merged.push_back(formatLine(t, trailingComment(raw)));
                    }
                    done[i] = true;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            merged.push_back(raw);
        }
    }

    // Mark dropped (omit-when-empty, blank) targets done so they neither force a
    // section to be created nor get appended.
    for (int i = 0; i < targets.size(); ++i) {
        if (!done[i] && !shouldEmit(targets[i])) {
            done[i] = true;
        }
    }

    auto sectionPresent = [&merged](const QString &section) -> bool {
        const QString header = QStringLiteral("[") + section + QStringLiteral("]");
        for (const QString &l : merged) {
            if (l.trimmed() == header) {
                return true;
            }
        }
        return false;
    };

    // Second pass: append any keys whose section exists but lacked the key, or
    // whose section is missing entirely. Group by section so each header is
    // emitted at most once, preserving target order.
    QStringList sectionsNeedingAppend;
    for (int i = 0; i < targets.size(); ++i) {
        if (!done[i] && !sectionsNeedingAppend.contains(targets[i].section)) {
            sectionsNeedingAppend.push_back(targets[i].section);
        }
    }
    for (const QString &section : sectionsNeedingAppend) {
        if (sectionPresent(section)) {
            const QString header = QStringLiteral("[") + section + QStringLiteral("]");
            QStringList rebuilt;
            for (const QString &l : merged) {
                rebuilt.push_back(l);
                if (l.trimmed() == header) {
                    for (int i = 0; i < targets.size(); ++i) {
                        if (!done[i] && targets[i].section == section) {
                            rebuilt.push_back(formatLine(targets[i], QString()));
                            done[i] = true;
                        }
                    }
                }
            }
            merged = rebuilt;
        } else {
            if (!merged.isEmpty() && !merged.last().trimmed().isEmpty()) {
                merged.push_back(QString());
            }
            merged.push_back(QStringLiteral("[") + section + QStringLiteral("]"));
            for (int i = 0; i < targets.size(); ++i) {
                if (!done[i] && targets[i].section == section) {
                    merged.push_back(formatLine(targets[i], QString()));
                    done[i] = true;
                }
            }
        }
    }

    return merged;
}

} // namespace voiceconfig
