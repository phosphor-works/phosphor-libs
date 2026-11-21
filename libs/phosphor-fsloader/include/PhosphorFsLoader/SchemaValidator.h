// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorFsLoader/phosphorfsloader_export.h>

#include <QtCore/QByteArray>
#include <QtCore/QJsonValue>
#include <QtCore/QList>
#include <QtCore/QString>

#include <memory>
#include <optional>

class QLoggingCategory;

namespace PhosphorFsLoader {

/**
 * @brief Validates parsed JSON documents against a JSON Schema (2020-12).
 *
 * The validator, jsoncons, is an implementation detail confined to the .cpp.
 * This header exposes only Qt and std types, so jsoncons never reaches the
 * public ABI or the consumers' includes.
 *
 * Construct one validator per document type from the serialized schema bytes
 * (typically a schema embedded as a Qt resource), compile it once, and reuse
 * it across every file of that type: compiling is the expensive step.
 * @c validate() costs what the document's size suggests: 70 ms for one of
 * 88000 members (2.4 MB as compact JSON) in a Release build.
 *
 * Failure model is fail-closed. If the schema is refused, @c isValid()
 * returns false and every @c validate() call reports a single error saying
 * why, rather than passing documents through unvalidated.
 *
 * What a schema may be:
 * - JSON as Qt reads it: no comments, and of two members with one name the
 *   last is kept, as Python's json module keeps it in CI. The root is an
 *   object (boolean schemas are allowed below it).
 * - 2020-12 only. @c $schema is absent or names
 *   @c https://json-schema.org/draft/2020-12/schema, with or without a
 *   trailing @c '#'. Any other spelling (@c http://, another draft) is
 *   refused, and so are the keywords of other drafts that 2020-12 would
 *   ignore: @c definitions, @c dependencies, @c additionalItems,
 *   @c $recursiveRef, @c $recursiveAnchor, and an array under @c items.
 *   Other unknown keywords are ignored, as 2020-12 says.
 * - Valid against the 2020-12 meta-schema, checked at construction, so a
 *   keyword with a value of the wrong kind (@c {"enum": "x"}, a negative
 *   @c maxItems, a @c pattern that is not an ECMAScript regex) is refused.
 * - References within one schema, which nothing is fetched for. A @c $ref is
 *   @c "#", @c "#/<JSON pointer>" or @c "#<anchor>", not percent-encoded,
 *   and names one of the schema's subschemas. An @c $anchor is defined once.
 *   @c $id is allowed at the root only, and @c $dynamicRef and
 *   @c $dynamicAnchor are refused.
 * - No @c $ref that leads back to its own schema without descending into the
 *   document, that is through @c allOf, @c anyOf, @c oneOf, @c not, @c if,
 *   @c then, @c else, @c dependentSchemas and other @c $ref s only, as
 *   @c {"$ref": "#"} does. jsoncons would recurse on it until the stack
 *   overflowed. It is refused even in a @c $defs entry nothing uses. A
 *   @c $ref back through @c properties, @c items or another keyword that
 *   applies to a member or an item is allowed: the document is finite.
 *
 * @c format is asserted, not only annotated, for the formats jsoncons
 * checks: date-time, date, time, email, ipv4, ipv6, regex, json-pointer, uri
 * and uri-reference, by jsoncons' reading of each, which is not always the
 * RFCs' nor Python's: a date may have the year 0000, an ipv4 address may have
 * an octet past 255 (256.1.1.1 passes), a uri-reference may not be empty, and
 * a uri may not end at its scheme (@c a: fails). A schema naming any other
 * format is refused, since jsoncons would ignore it, and so is "hostname",
 * whose jsoncons check refuses the digit 9 and reads past the end of an
 * empty string.
 *
 * What a document may hold: a string or member name with U+0000 or an
 * unpaired UTF-16 surrogate, or a number outside int64 (below -2^63, or 2^63
 * and above, -2^63 itself being allowed), is refused at its path before
 * jsoncons sees it (the same holds in the schema). That holds wherever the
 * value sits, also where the schema accepts any value or says nothing: the
 * check runs over the whole document first. jsoncons reads strings as UTF-8
 * and names through C strings, so U+0000 would cut them short and two names
 * with different unpaired surrogates would become one. Its integer check
 * converts a double to int64, which is undefined outside int64, and it may
 * run at any node. CI's Python accepts such a number under "number", or under
 * no constraint at all, so there the load is stricter than CI.
 *
 * How the keywords read, where jsoncons differs from Python's 2020-12:
 * - Numbers: an integer within int64 is exact, 3.0 included (2020-12 calls
 *   it an integer), and so are @c minimum, @c maximum and their exclusive
 *   forms between integers. A literal that is not exact as a double nor an
 *   int64 is rounded by Qt first: -9223372036854775809 is read as the int64
 *   minimum. @c multipleOf works in doubles and passes a value within one ulp
 *   of a multiple, so 0.07 is a multiple of 0.01 here and not in Python, and
 *   an odd integer past 2^53 can pass @c {"multipleOf": 2}. Its message
 *   prints the divisor with six decimals ("0.075 is not a multiple of
 *   0.010000"), so a divisor of 0.0000001 reads as 0.000000.
 * - @c pattern is an ECMAScript regex (std::regex) matched against UTF-8
 *   bytes, not characters: a dot, a class or a quantifier applies to one byte
 *   of a character past U+007F, so @c ^.$ does not match "é" and @c \\s does
 *   not match U+00A0. (U+0000 never reaches it: it is refused before.)
 */
class PHOSPHORFSLOADER_EXPORT SchemaValidator
{
public:
    /// A single schema violation.
    struct Error
    {
        /// JSON Pointer to the node that failed validation (e.g.
        /// "/zones/0/relativeGeometry/width"). Empty for a document-root
        /// failure. A failed @c additionalProperties subschema is placed at
        /// the member it names. A failed @c propertyNames, @c contains or
        /// @c unevaluatedProperties subschema is reported at the object or
        /// array, as jsoncons reports it. A failed @c dependentSchemas
        /// subschema is reported below the member that triggered it, also
        /// jsoncons' way: with @c {"dependentSchemas": {"a": {"properties":
        /// {"c": {"type": "string"}}}}}, the document @c {"a": 1, "c": 2}
        /// fails at "/a/c", not "/c". A member that fails an
        /// @c additionalProperties subschema below @c dependentSchemas keeps
        /// jsoncons' line only, without the reasons @c message describes,
        /// since that location does not name the object validated. The
        /// characters that could forge a log line are escaped in the path, as
        /// in @c message.
        QString path;

        /// Human-readable description of why the node failed. The reasons
        /// an @c anyOf, @c oneOf or @c allOf failed follow it as errors of
        /// their own, each prefixed with its branch ("anyOf/1: ..."). So do
        /// the reasons a member failed an @c additionalProperties subschema,
        /// which jsoncons keeps to itself: the member is checked against the
        /// subschema again, and each reason is prefixed
        /// "additionalProperties: ". That goes four such subschemas deep (a
        /// member of a member of ...), which bounds what it costs: at most
        /// four more passes over the document.
        ///
        /// In the path and in the message, each C0 and C1 control character
        /// (U+0000 to U+001F, U+007F to U+009F), the line and paragraph
        /// separators U+2028 and U+2029, each bidirectional embedding,
        /// override and isolate (U+202A to U+202E, U+2066 to U+2069) and an
        /// unpaired surrogate is written as @c \\uXXXX in lower-case hex. So a
        /// member name, or a string a message quotes from a data file, can
        /// neither start a log line of its own, nor send a terminal escape
        /// sequence, nor reorder the text around it. A backslash is kept as
        /// it is, so a name holding the six characters @c \\u000a reads the
        /// same as one holding a newline.
        ///
        /// A path or a message longer than 500 UTF-16 code units, escapes
        /// included, keeps its first 500 or a few fewer, so as not to split
        /// a surrogate pair or an escape, and an ellipsis (U+2026) is
        /// appended: 501 code units at most.
        QString message;
    };

    /**
     * @brief Compile a JSON Schema from its serialized bytes.
     *
     * @param schemaJson  UTF-8 JSON text of a 2020-12 schema. A schema that
     *                    is refused (see the class) or does not compile
     *                    leaves the validator in an invalid state
     *                    (@c isValid() == false) instead of throwing.
     */
    explicit SchemaValidator(const QByteArray& schemaJson);
    ~SchemaValidator();

    SchemaValidator(SchemaValidator&&) noexcept;
    SchemaValidator& operator=(SchemaValidator&&) noexcept;
    SchemaValidator(const SchemaValidator&) = delete;
    SchemaValidator& operator=(const SchemaValidator&) = delete;

    /**
     * @brief Build a validator from a Qt resource (or file) path, failing closed.
     *
     * Opens @p resourcePath and compiles the schema from its bytes. If the
     * resource cannot be opened — for an RCC-embedded schema this means a build
     * regression — it logs a warning at @p category and returns a validator
     * whose @c isValid() is false, so every @c validate() call rejects rather
     * than silently passing documents through unvalidated. The single place the
     * embed-and-fail-closed contract lives, shared by every loader.
     */
    [[nodiscard]] static SchemaValidator fromResource(const QString& resourcePath, const QLoggingCategory& category);

    /// True when the schema compiled successfully and the validator is usable.
    /// False for a validator that was moved from, whose @c validate() fails
    /// closed.
    [[nodiscard]] bool isValid() const;

    /**
     * @brief Validate a parsed JSON document against the compiled schema.
     *
     * @param document  Any parsed JSON value (typically a QJsonObject).
     * @return @c std::nullopt when the document satisfies the schema;
     *         otherwise the non-empty list of violations, at most 100 of
     *         them: when there were more, one last entry with an empty path
     *         says how many more ("and 250 more errors not listed"). Past the
     *         100th, the reasons of a failed @c additionalProperties member
     *         are not looked for, so the count holds what jsoncons reported.
     *         A validator whose schema was refused always returns a
     *         one-element list, whose message says why (for a schema the
     *         meta-schema refuses, its most specific reason, at the node of
     *         the schema it names).
     */
    [[nodiscard]] std::optional<QList<Error>> validate(const QJsonValue& document) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

/**
 * @brief Log each schema violation at @p category, one per line.
 *
 * Each error is logged as "  <json-pointer>: <message>", unquoted, with
 * "(root)" for a document-root failure, and the count @c validate() appends
 * past its 100th error as "  <message>". The path and the message are
 * escaped as @c SchemaValidator::Error describes, which changes nothing in
 * what @c validate() returned (it is escaped already) and keeps an error
 * built elsewhere from logging a control character. Centralizes the
 * per-error diagnostic format every loader shares. Callers emit their own
 * "skipping X" headline first.
 */
PHOSPHORFSLOADER_EXPORT void logSchemaErrors(const QLoggingCategory& category,
                                             const QList<SchemaValidator::Error>& errors);

} // namespace PhosphorFsLoader
