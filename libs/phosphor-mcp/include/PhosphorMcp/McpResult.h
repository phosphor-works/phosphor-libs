// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorMcp/phosphormcp_export.h>

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace PhosphorMcp {

// One content block of a tool result: text, or an image that goes out as
// base64 with its MIME type.
class PHOSPHORMCP_EXPORT McpContent
{
public:
    enum class Type {
        Text,
        Image,
    };

    [[nodiscard]] static McpContent text(const QString& text);
    // `bytes` are the encoded image (a PNG, a JPEG), not base64: the
    // library encodes them.
    [[nodiscard]] static McpContent image(const QByteArray& bytes, const QString& mimeType);

    [[nodiscard]] Type type() const;
    [[nodiscard]] QString text() const;
    [[nodiscard]] QByteArray imageBytes() const;
    [[nodiscard]] QString mimeType() const;

    [[nodiscard]] QJsonObject toJson() const;

private:
    McpContent() = default;

    Type m_type = Type::Text;
    QString m_text;
    QByteArray m_bytes;
    QString m_mimeType;
};

// What a tool answers: content blocks, an optional structured value, and
// whether the call failed. A failed call (isError) is a result the model
// reads and can act on, not a protocol error: bad arguments, a pane that
// does not exist, a refusal.
//
// A result with structured content also carries the compact JSON of that
// value as its last text block, which the server appends when it sends the
// result, for clients that read only the content. Do not add that text
// yourself.
class PHOSPHORMCP_EXPORT McpResult
{
public:
    McpResult() = default;

    [[nodiscard]] static McpResult text(const QString& text);
    // A failed call with one text block that tells the model what went
    // wrong and what to do.
    [[nodiscard]] static McpResult error(const QString& text);
    [[nodiscard]] static McpResult structured(const QJsonObject& value);

    McpResult& addText(const QString& text);
    McpResult& addImage(const QByteArray& bytes, const QString& mimeType);
    McpResult& addContent(const McpContent& content);
    McpResult& setStructuredContent(const QJsonObject& value);
    McpResult& setError(bool isError);

    [[nodiscard]] const QList<McpContent>& content() const;
    [[nodiscard]] bool hasStructuredContent() const;
    [[nodiscard]] QJsonObject structuredContent() const;
    [[nodiscard]] bool isError() const;

    // The result object without resultType and _meta, which the server
    // adds: content (with the structured value's text appended),
    // structuredContent, and isError when it is true.
    [[nodiscard]] QJsonObject toJson() const;

private:
    QList<McpContent> m_content;
    QJsonObject m_structured;
    bool m_hasStructured = false;
    bool m_isError = false;
};

} // namespace PhosphorMcp
