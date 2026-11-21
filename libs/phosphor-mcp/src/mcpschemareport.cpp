// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The lines of the errors: JSON pointers, names cut short, and the list
// with its caps.

#include "mcpschemacheck.h"

#include <QLatin1StringView>

#include <algorithm>
#include <charconv>
#include <iterator>
#include <type_traits>

namespace PhosphorMcp::Schema {

namespace {

const QString TooLargeToList =
    QStringLiteral(": does not match the schema, and is too large for the server to list each error");
// The walk before the check does not know whether the value matches.
const QString TooManyRefused =
    QStringLiteral(": is refused before the check, with too many errors for the server to list each one");

// The UTF-16 code units that a text takes at the least. A byte of UTF-8 is
// a third of one at the least.
qsizetype leastUtf16(QAnyStringView text)
{
    return text.visit([](auto view) -> qsizetype {
        if constexpr (std::is_same_v<decltype(view), QStringView>
                      || std::is_same_v<decltype(view), QLatin1StringView>) {
            return view.size();
        } else {
            return view.size() / 3;
        }
    });
}

// The code points of a name that shownName() reads at most.
constexpr qsizetype ShownPoints = MaxShownTokenLength + 1;

// Adds `text` to `out`, decoded.
void appendDecoded(QString& out, QAnyStringView text)
{
    text.visit([&out](auto view) {
        if constexpr (std::is_same_v<decltype(view), QStringView>
                      || std::is_same_v<decltype(view), QLatin1StringView>) {
            out += view;
        } else {
            out += QUtf8StringView(view.data(), view.size());
        }
    });
}

// The start of a name: all of it, or enough code points (more than
// MaxShownTokenLength) that appendShown() cuts it where it cuts the whole
// name.
QAnyStringView headOf(QAnyStringView name)
{
    return name.visit([](auto view) -> QAnyStringView {
        using View = decltype(view);
        if constexpr (std::is_same_v<View, QStringView>) {
            return view.first(std::min(view.size(), 2 * ShownPoints));
        } else if constexpr (std::is_same_v<View, QLatin1StringView>) {
            return view.first(std::min(view.size(), ShownPoints));
        } else {
            // Up to four bytes a code point, cut at the start of one.
            const auto* bytes = reinterpret_cast<const uchar*>(view.data());
            qsizetype size = std::min(view.size(), 4 * ShownPoints);
            while (size < view.size() && (bytes[size] & 0xC0) == 0x80) {
                --size;
            }
            return view.first(size);
        }
    });
}

// Adds `name` to `out` as an error shows it (see shownName()).
void appendShown(QString& out, QStringView name)
{
    // Only the start of a long name is shown, so only that much is copied.
    const qsizetype start = out.size();
    out += name.first(std::min(name.size(), MaxShownTokenLength + 1));
    QChar* token = out.data() + start;
    const qsizetype size = out.size() - start;
    for (qsizetype i = 0; i < size; ++i) {
        const QChar c = token[i];
        if (c.isHighSurrogate() && i + 1 < size && token[i + 1].isLowSurrogate()) {
            ++i;
        } else if (c.isSurrogate()) {
            token[i] = QChar(QChar::ReplacementCharacter);
        }
    }
    if (name.size() > MaxShownTokenLength) {
        out.truncate(start + MaxShownTokenLength - 3);
        // Neither half of a surrogate pair nor of an escape is left alone.
        if (out.back().isHighSurrogate() || out.back() == QLatin1Char('~')) {
            out.chop(1);
        }
        out += QLatin1StringView("...");
    }
}

} // namespace

qsizetype escapeToken(QString& text, qsizetype start)
{
    qsizetype escapes = 0;
    const QChar* chars = text.constData();
    for (qsizetype i = start; i < text.size(); ++i) {
        escapes += chars[i] == QLatin1Char('~') || chars[i] == QLatin1Char('/') ? 1 : 0;
    }
    if (escapes == 0) {
        return 0;
    }
    // Each character moved to its place from the back, until those before
    // the first escape, which stay.
    qsizetype from = text.size();
    text.resize(from + escapes);
    QChar* out = text.data();
    qsizetype to = text.size();
    while (to > from) {
        const QChar c = out[--from];
        if (c == QLatin1Char('~') || c == QLatin1Char('/')) {
            out[--to] = c == QLatin1Char('~') ? QLatin1Char('0') : QLatin1Char('1');
            out[--to] = QLatin1Char('~');
        } else {
            out[--to] = c;
        }
    }
    return escapes;
}

QString pointerToken(const QString& name)
{
    QString token = name;
    escapeToken(token, 0);
    return token;
}

QString shownName(const QString& name)
{
    QString token;
    appendShown(token, name);
    return token;
}

void Place::pop()
{
    m_tokens.pop_back();
    if (m_ends.size() > m_tokens.size() + 1) {
        m_ends.resize(m_tokens.size() + 1);
        m_pointer.truncate(m_ends.back());
    }
}

const QString* Place::pointer(qint64& units)
{
    for (std::size_t k = m_ends.size() - 1; k < m_tokens.size(); ++k) {
        const PathToken& token = m_tokens[k];
        units += UnitsPerToken;
        if (token.index >= 0) {
            char digits[24];
            const char* end = std::to_chars(std::begin(digits), std::end(digits), token.index).ptr;
            m_pointer += QLatin1Char('/');
            m_pointer += QLatin1StringView(std::begin(digits), end);
        } else {
            // A name too long for any pointer that the report keeps is not
            // decoded.
            if (m_pointer.size() + 1 + leastUtf16(token.name) > MaxErrorPointerLength) {
                return nullptr;
            }
            m_pointer += QLatin1Char('/');
            const qsizetype start = m_pointer.size();
            appendDecoded(m_pointer, token.name);
            // Decoded, scanned for "~" and "/", and written again from the
            // first escape.
            const qsizetype added = m_pointer.size() - start;
            units += decodeUnits(token.name) + (added + escapeToken(m_pointer, start)) / CharactersPerUnit;
        }
        m_ends.push_back(m_pointer.size());
    }
    return m_pointer.size() > MaxErrorPointerLength ? nullptr : &m_pointer;
}

qint64 Report::add(Place& place, const QString& description)
{
    if (m_overflow) {
        return 0;
    }
    qint64 units = 0;
    const QString* pointer = place.pointer(units);
    if (!pointer) {
        m_overflow = true;
        return units;
    }
    // The pointer hashed, compared with those of its hash, and copied when
    // it is new.
    units += pointer->size() / CharactersPerUnit;
    if (m_seen.contains(Seen{*pointer, description})) {
        return units;
    }
    if (m_seen.size() >= MaxCountedErrors || m_pointerLength + pointer->size() > MaxErrorPointerLength) {
        m_overflow = true;
        return units;
    }
    // A copy of its own, since the place's pointer changes as the walk goes.
    m_seen.insert(Seen{QString(pointer->constData(), pointer->size()), description});
    m_pointerLength += pointer->size();
    if (m_lines.size() < MaxListedErrors) {
        QString shown;
        // The start of each name decoded and escaped, then cut, in a
        // buffer of its own.
        QString head;
        for (const PathToken& token : place.tokens()) {
            units += UnitsPerShownToken;
            shown += QLatin1Char('/');
            if (token.index >= 0) {
                char digits[24];
                const char* end = std::to_chars(std::begin(digits), std::end(digits), token.index).ptr;
                shown += QLatin1StringView(std::begin(digits), end);
            } else {
                head.truncate(0);
                appendDecoded(head, headOf(token.name));
                const qsizetype added = head.size();
                units += (added + escapeToken(head, 0)) / CharactersPerUnit;
                appendShown(shown, head);
            }
        }
        m_lines.append(shown + QLatin1StringView(": ") + description);
    } else {
        ++m_more;
    }
    return units;
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
