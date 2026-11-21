// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The lines of the errors: JSON pointers, names cut short, and the list
// with its caps.

#include "mcpschemacheck.h"

#include <QLatin1StringView>

namespace PhosphorMcp::Schema {

namespace {

const QString TooLargeToList =
    QStringLiteral(": does not match the schema, and is too large for the server to list each error");
// The walk before the check does not know whether the value matches.
const QString TooManyRefused =
    QStringLiteral(": is refused before the check, with too many errors for the server to list each one");

QString tokenOf(const PathToken& token)
{
    return token.index >= 0 ? QString::number(token.index) : token.name.toString();
}

} // namespace

// A name without "~" or "/" is returned as it is, since QString::replace()
// costs even when it finds nothing.
QString pointerToken(const QString& name)
{
    if (!name.contains(QLatin1Char('~')) && !name.contains(QLatin1Char('/'))) {
        return name;
    }
    QString token = name;
    token.replace(QLatin1Char('~'), QLatin1StringView("~0"));
    token.replace(QLatin1Char('/'), QLatin1StringView("~1"));
    return token;
}

QString shownName(const QString& name)
{
    // Only the start of a long name is shown, so only that much is copied.
    QString token = name.left(MaxShownTokenLength + 1);
    for (qsizetype i = 0; i < token.size(); ++i) {
        const QChar c = token.at(i);
        if (c.isHighSurrogate() && i + 1 < token.size() && token.at(i + 1).isLowSurrogate()) {
            ++i;
        } else if (c.isSurrogate()) {
            token[i] = QChar(QChar::ReplacementCharacter);
        }
    }
    if (name.size() > MaxShownTokenLength) {
        token.truncate(MaxShownTokenLength - 3);
        // Neither half of a surrogate pair nor of an escape is left alone.
        if (token.back().isHighSurrogate() || token.back() == QLatin1Char('~')) {
            token.chop(1);
        }
        token += QLatin1StringView("...");
    }
    return token;
}

qsizetype Report::add(const Path& path, const QString* last, const QString& description)
{
    if (m_overflow) {
        return 0;
    }
    QString pointer;
    for (const PathToken& token : path) {
        pointer += QLatin1Char('/') + pointerToken(tokenOf(token));
    }
    if (last) {
        pointer += QLatin1Char('/') + pointerToken(*last);
    }
    const QString key = pointer + QLatin1StringView(": ") + description;
    if (m_seen.contains(key)) {
        return pointer.size();
    }
    if (m_seen.size() >= MaxCountedErrors || m_pointerLength + pointer.size() > MaxErrorPointerLength) {
        m_overflow = true;
        return pointer.size();
    }
    m_seen.insert(key);
    m_pointerLength += pointer.size();
    if (m_lines.size() < MaxListedErrors) {
        QString shown;
        for (const PathToken& token : path) {
            shown += QLatin1Char('/') + shownName(pointerToken(tokenOf(token)));
        }
        if (last) {
            shown += QLatin1Char('/') + shownName(pointerToken(*last));
        }
        m_lines.append(shown + QLatin1StringView(": ") + description);
    } else {
        ++m_more;
    }
    return pointer.size();
}

QStringList Report::take()
{
    if (m_overflow) {
        return {m_kind == Kind::Precheck ? TooManyRefused : TooLargeToList};
    }
    QStringList lines = std::move(m_lines);
    if (m_more == 1) {
        lines.append(QStringLiteral(": and 1 more error"));
    } else if (m_more > 1) {
        lines.append(QStringLiteral(": and %1 more errors").arg(m_more));
    }
    return lines;
}

QString nodePointer(const std::vector<Node>& nodes, int index)
{
    QStringList tokens;
    for (int i = index; i >= 0 && nodes[std::size_t(i)].parent >= 0; i = nodes[std::size_t(i)].parent) {
        const Node& node = nodes[std::size_t(i)];
        if (node.hasName) {
            tokens.prepend(node.name);
        }
        tokens.prepend(node.keyword);
    }
    QString pointer;
    for (const QString& token : std::as_const(tokens)) {
        pointer += QLatin1Char('/') + pointerToken(token);
    }
    return pointer;
}

} // namespace PhosphorMcp::Schema
