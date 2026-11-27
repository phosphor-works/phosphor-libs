// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "subcommands.h"

#include <QChar>
#include <QCoreApplication>
#include <QLatin1String>
#include <QLatin1StringView>

namespace PhosphorIpcCli {

namespace {
// Common control-byte escape shared by both sanitisers. Returns
// `\xNN` for any input that should be escaped; the caller decides
// whether to escape a given QChar.
QString escapeControl(ushort u)
{
    return QStringLiteral("\\x%1").arg(u, 2, 16, QLatin1Char('0'));
}
} // namespace

QString sanitiseForTerminal(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        const ushort u = c.unicode();
        // Pass through '\t' and '\n' verbatim, those are
        // structurally meaningful for textual output (tab columns,
        // newlines between lines). Escape everything else below
        // 0x20 (the ASCII control range that includes ESC=0x1B,
        // the lead byte for ANSI/CSI/OSC terminal sequences) as a
        // visible `\xNN` literal. We deliberately do NOT escape
        // 0x7F (DEL): it's a benign legacy control byte that
        // doesn't lead a terminal escape sequence, and matching
        // JSON spec (which only escapes 0x00-0x1F) keeps the
        // string and JSON output paths symmetric.
        if (u == 0x09 || u == 0x0A) {
            out.append(c);
        } else if (u < 0x20) {
            out.append(escapeControl(u));
        } else {
            out.append(c);
        }
    }
    return out;
}

QString sanitiseForSingleLine(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        const ushort u = c.unicode();
        // Strict variant: escape every ASCII control byte INCLUDING
        // tab, newline, and carriage return so a server-supplied
        // string can't visually break out of the surrounding
        // single-line diagnostic frame on stderr.
        if (u < 0x20) {
            out.append(escapeControl(u));
        } else {
            out.append(c);
        }
    }
    return out;
}

namespace {
// Strips every `<name> VALUE` / `<name>=VALUE` (and `<shortName> VALUE`
// when it is set) from `args` and returns the last value, or empty when
// the option is not there. `valueName` names the value in the errors.
QString stripOption(QStringList& args, QLatin1StringView name, QLatin1StringView shortName, QLatin1StringView valueName,
                    QString* errorMessage)
{
    const QString withValue = name + QLatin1Char('=');
    QString resolved;
    for (int i = 0; i < args.size();) {
        const QString& a = args.at(i);
        if (a == name || (!shortName.isEmpty() && a == shortName)) {
            if (i + 1 >= args.size()) {
                // Dangling flag with no value. Surface as a usage
                // error rather than leaving the bare flag in args
                // (where the subcommand handler would otherwise treat
                // it as a positional argument — schema would try to
                // query target "--socket", call would error on the
                // dot-form check, etc.).
                if (errorMessage) {
                    *errorMessage = QStringLiteral("'%1' requires a %2 argument").arg(a, valueName);
                }
                args.removeAt(i);
                return {};
            }
            resolved = args.at(i + 1);
            args.removeAt(i + 1);
            args.removeAt(i);
            continue;
        }
        if (a.startsWith(withValue)) {
            resolved = a.mid(withValue.size());
            args.removeAt(i);
            if (resolved.isEmpty()) {
                // `--socket=` with empty value: same usage-error
                // surface as the dangling-flag case above. Don't
                // silently fall through to the defaults — the user
                // expected the override to take effect.
                if (errorMessage) {
                    *errorMessage = QStringLiteral("'%1' requires a non-empty %2").arg(withValue, valueName);
                }
                return {};
            }
            continue;
        }
        ++i;
    }
    return resolved;
}
} // namespace

QString stripSocketFlag(QStringList& args, QString* errorMessage)
{
    return stripOption(args, QLatin1StringView("--socket"), QLatin1StringView("-s"), QLatin1StringView("path"),
                       errorMessage);
}

QString programName()
{
    return QCoreApplication::applicationName();
}

} // namespace PhosphorIpcCli
