// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorMcp/phosphormcp_export.h>

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>

namespace PhosphorMcp {

// One content block of a tool result: text, an image or an audio clip that
// goes out as base64 with its MIME type, or any other block of the spec
// given as its JSON.
class PHOSPHORMCP_EXPORT McpContent
{
public:
    enum class Type {
        Text,
        Image,
        Audio,
        // A block given whole to fromJson().
        Json,
    };

    [[nodiscard]] static McpContent text(const QString& text);
    // `bytes` are the encoded image (a PNG, a JPEG), not base64. The library
    // encodes them when the result goes out. Base64 takes 4/3 of the bytes,
    // and the JSON objects and the serialized reply each hold that text again
    // while the result is built and written. So a large image raises the peak
    // memory by about eight times its size, briefly (133 MiB for 16 MiB,
    // measured), and the queued reply holds 4/3 of it until it is sent.
    [[nodiscard]] static McpContent image(const QByteArray& bytes, const QString& mimeType);
    // The same for an encoded audio clip (a WAV, an MP3).
    [[nodiscard]] static McpContent audio(const QByteArray& bytes, const QString& mimeType);
    // Any other block, sent as it is: a resource_link, an embedded resource,
    // a block with annotations. The caller answers for its shape, and the
    // result refuses only a block without a string "type".
    [[nodiscard]] static McpContent fromJson(const QJsonObject& block);

    [[nodiscard]] Type type() const;
    // The text of a Text block, else empty.
    [[nodiscard]] QString text() const;
    // The encoded bytes of an Image or Audio block, else empty.
    [[nodiscard]] QByteArray bytes() const;
    // The MIME type of an Image or Audio block, else empty.
    [[nodiscard]] QString mimeType() const;

    // The block as it goes out.
    [[nodiscard]] QJsonObject toJson() const;

private:
    McpContent() = default;

    Type m_type = Type::Text;
    QString m_text;
    QByteArray m_bytes;
    QString m_mimeType;
    QJsonObject m_json;
};

// What a tool answers: content blocks, an optional structured value, and
// whether the call failed. A failed call (isError) is a result the model
// reads and can act on, not a protocol error. Bad arguments, a pane that
// does not exist and a refusal are failed calls.
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
    // A result whose structured content is `value`, which may be any JSON
    // value (an object, in most tools).
    [[nodiscard]] static McpResult structured(const QJsonValue& value);

    McpResult& addText(const QString& text);
    McpResult& addImage(const QByteArray& bytes, const QString& mimeType);
    McpResult& addAudio(const QByteArray& bytes, const QString& mimeType);
    // Adds the block. An Image or Audio block with no bytes or no MIME type,
    // and a Json block without a string "type", are not added, with a
    // warning, since a client would refuse them.
    McpResult& addContent(const McpContent& content);
    // Any JSON value. An undefined QJsonValue removes the structured content.
    // A number that is not finite has no JSON form, so the server refuses it
    // when the tool has an output schema, and warns when it has none.
    McpResult& setStructuredContent(const QJsonValue& value);
    McpResult& setError(bool isError);

    [[nodiscard]] const QList<McpContent>& content() const;
    [[nodiscard]] bool hasStructuredContent() const;
    // The structured content, or an undefined QJsonValue when there is none.
    [[nodiscard]] QJsonValue structuredContent() const;
    [[nodiscard]] bool isError() const;

    // The result object without resultType and _meta, which the server
    // adds: content (with the structured value's text appended),
    // structuredContent, and isError when it is true.
    [[nodiscard]] QJsonObject toJson() const;

private:
    QList<McpContent> m_content;
    QJsonValue m_structured = QJsonValue(QJsonValue::Undefined);
    bool m_isError = false;
};

} // namespace PhosphorMcp
