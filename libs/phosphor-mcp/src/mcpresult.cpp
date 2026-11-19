// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcp/McpResult.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QLatin1StringView>

namespace PhosphorMcp {

McpContent McpContent::text(const QString& text)
{
    McpContent content;
    content.m_type = Type::Text;
    content.m_text = text;
    return content;
}

McpContent McpContent::image(const QByteArray& bytes, const QString& mimeType)
{
    McpContent content;
    content.m_type = Type::Image;
    content.m_bytes = bytes;
    content.m_mimeType = mimeType;
    return content;
}

McpContent::Type McpContent::type() const
{
    return m_type;
}

QString McpContent::text() const
{
    return m_text;
}

QByteArray McpContent::imageBytes() const
{
    return m_bytes;
}

QString McpContent::mimeType() const
{
    return m_mimeType;
}

QJsonObject McpContent::toJson() const
{
    if (m_type == Type::Image) {
        return QJsonObject{
            {QLatin1StringView("type"), QStringLiteral("image")},
            {QLatin1StringView("data"), QString::fromLatin1(m_bytes.toBase64())},
            {QLatin1StringView("mimeType"), m_mimeType},
        };
    }
    return QJsonObject{
        {QLatin1StringView("type"), QStringLiteral("text")},
        {QLatin1StringView("text"), m_text},
    };
}

McpResult McpResult::text(const QString& text)
{
    McpResult result;
    result.addText(text);
    return result;
}

McpResult McpResult::error(const QString& text)
{
    McpResult result;
    result.addText(text);
    result.setError(true);
    return result;
}

McpResult McpResult::structured(const QJsonObject& value)
{
    McpResult result;
    result.setStructuredContent(value);
    return result;
}

McpResult& McpResult::addText(const QString& text)
{
    m_content.append(McpContent::text(text));
    return *this;
}

McpResult& McpResult::addImage(const QByteArray& bytes, const QString& mimeType)
{
    m_content.append(McpContent::image(bytes, mimeType));
    return *this;
}

McpResult& McpResult::addContent(const McpContent& content)
{
    m_content.append(content);
    return *this;
}

McpResult& McpResult::setStructuredContent(const QJsonObject& value)
{
    m_structured = value;
    m_hasStructured = true;
    return *this;
}

McpResult& McpResult::setError(bool isError)
{
    m_isError = isError;
    return *this;
}

const QList<McpContent>& McpResult::content() const
{
    return m_content;
}

bool McpResult::hasStructuredContent() const
{
    return m_hasStructured;
}

QJsonObject McpResult::structuredContent() const
{
    return m_structured;
}

bool McpResult::isError() const
{
    return m_isError;
}

QJsonObject McpResult::toJson() const
{
    QJsonArray content;
    for (const McpContent& block : m_content) {
        content.append(block.toJson());
    }
    QJsonObject result;
    if (m_hasStructured) {
        // The spec asks a tool that returns structured content to return
        // its serialized JSON as text too, for clients that do not read
        // structuredContent.
        content.append(
            McpContent::text(QString::fromUtf8(QJsonDocument(m_structured).toJson(QJsonDocument::Compact))).toJson());
        result.insert(QLatin1StringView("structuredContent"), m_structured);
    }
    result.insert(QLatin1StringView("content"), content);
    if (m_isError) {
        result.insert(QLatin1StringView("isError"), true);
    }
    return result;
}

} // namespace PhosphorMcp
