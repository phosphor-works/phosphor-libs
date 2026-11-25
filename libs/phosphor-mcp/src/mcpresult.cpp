// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorMcp/McpResult.h>

#include "mcplogging.h"

#include <QJsonArray>
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

McpContent McpContent::audio(const QByteArray& bytes, const QString& mimeType)
{
    McpContent content;
    content.m_type = Type::Audio;
    content.m_bytes = bytes;
    content.m_mimeType = mimeType;
    return content;
}

McpContent McpContent::fromJson(const QJsonObject& block)
{
    McpContent content;
    content.m_type = Type::Json;
    content.m_json = block;
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

QByteArray McpContent::bytes() const
{
    return m_bytes;
}

QString McpContent::mimeType() const
{
    return m_mimeType;
}

QJsonObject McpContent::toJson() const
{
    switch (m_type) {
    case Type::Image:
    case Type::Audio: {
        // Base64 is ASCII. As a Latin-1 view it goes into the JSON value as
        // it is, without a UTF-16 copy of twice its size.
        const QByteArray base64 = m_bytes.toBase64();
        return QJsonObject{
            {QLatin1StringView("type"), m_type == Type::Image ? QStringLiteral("image") : QStringLiteral("audio")},
            {QLatin1StringView("data"), QJsonValue(QLatin1StringView(base64))},
            {QLatin1StringView("mimeType"), m_mimeType},
        };
    }
    case Type::Json:
        return m_json;
    case Type::Text:
        break;
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

McpResult McpResult::structured(const QJsonValue& value)
{
    McpResult result;
    result.setStructuredContent(value);
    return result;
}

McpResult& McpResult::addText(const QString& text)
{
    return addContent(McpContent::text(text));
}

McpResult& McpResult::addImage(const QByteArray& bytes, const QString& mimeType)
{
    return addContent(McpContent::image(bytes, mimeType));
}

McpResult& McpResult::addAudio(const QByteArray& bytes, const QString& mimeType)
{
    return addContent(McpContent::audio(bytes, mimeType));
}

McpResult& McpResult::addContent(const McpContent& content)
{
    switch (content.type()) {
    case McpContent::Type::Image:
    case McpContent::Type::Audio:
        if (content.bytes().isEmpty() || content.mimeType().isEmpty()) {
            qCWarning(lcMcp).noquote()
                << "McpResult: an image or audio block needs bytes and a MIME type, so this one is not added";
            return *this;
        }
        break;
    case McpContent::Type::Json:
        if (!content.toJson().value(QLatin1StringView("type")).isString()) {
            qCWarning(lcMcp).noquote()
                << "McpResult: a content block needs a string \"type\", so this one is not added";
            return *this;
        }
        break;
    case McpContent::Type::Text:
        break;
    }
    m_content.append(content);
    return *this;
}

McpResult& McpResult::setStructuredContent(const QJsonValue& value)
{
    m_structured = value;
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
    return !m_structured.isUndefined();
}

QJsonValue McpResult::structuredContent() const
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
    if (hasStructuredContent()) {
        // The spec asks a tool that returns structured content to return
        // its serialized JSON as text too, for clients that do not read
        // structuredContent.
        content.append(
            McpContent::text(QString::fromUtf8(m_structured.toJson(QJsonValue::JsonFormat::Compact))).toJson());
        result.insert(QLatin1StringView("structuredContent"), m_structured);
    }
    result.insert(QLatin1StringView("content"), content);
    if (m_isError) {
        result.insert(QLatin1StringView("isError"), true);
    }
    return result;
}

} // namespace PhosphorMcp
