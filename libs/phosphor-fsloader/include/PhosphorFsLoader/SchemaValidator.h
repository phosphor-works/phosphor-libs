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
 * 88000 members (1.5 MB) in a Release build.
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
 * - A @c $ref within the schema: nothing is fetched.
 *
 * @c format is asserted, not only annotated, for the formats jsoncons
 * checks: date-time, date, time, email, ipv4, ipv6, regex, json-pointer, uri
 * and uri-reference, by jsoncons' reading of each (it takes the year 0000 in
 * a date, which Python refuses). A schema naming any other format is refused,
 * since jsoncons would ignore it, and so is "hostname", whose jsoncons check
 * refuses the digit 9 and reads past the end of an empty string.
 *
 * What a document may hold: a string or member name with U+0000 or an
 * unpaired UTF-16 surrogate, or a number of magnitude 2^63 or more, is
 * refused at its path before jsoncons sees it (the same holds in the schema).
 * jsoncons reads strings as UTF-8 and names through C strings, so U+0000
 * would cut them short and two names with different unpaired surrogates
 * would become one; and its integer check is undefined past int64. CI's
 * Python accepts such numbers under "number", so there the load is stricter.
 *
 * How the keywords read, where jsoncons differs from Python's 2020-12:
 * - Numbers: an integer within int64 is exact, 3.0 included (2020-12 calls
 *   it an integer), and so are @c minimum, @c maximum and their exclusive
 *   forms between integers. A literal that is not exact as a double nor an
 *   int64 is rounded by Qt first: -9223372036854775809 is read as the int64
 *   minimum. @c multipleOf works in doubles and passes a value within one ulp
 *   of a multiple, so 0.07 is a multiple of 0.01 here and not in Python, and
 *   an odd integer past 2^53 can pass @c {"multipleOf": 2}.
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
        /// array, as jsoncons reports it.
        QString path;

        /// Human-readable description of why the node failed. The reasons
        /// an @c anyOf, @c oneOf or @c allOf failed follow it as errors of
        /// their own, each prefixed with its branch ("anyOf/1: ...").
        /// Path and message are each cut to 500 characters.
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
     *         says how many more ("and 250 more errors not listed"). A
     *         validator whose schema was refused always returns a
     *         one-element list.
     */
    [[nodiscard]] std::optional<QList<Error>> validate(const QJsonValue& document) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

/**
 * @brief Log each schema violation at @p category, one per line.
 *
 * Each error is logged as "  <json-pointer>: <message>" (or "(root)" for a
 * document-root failure), and the count @c validate() appends past its 100th
 * error as "  <message>". Centralizes the per-error diagnostic format every
 * loader shares; callers emit their own "skipping X" headline first.
 */
PHOSPHORFSLOADER_EXPORT void logSchemaErrors(const QLoggingCategory& category,
                                             const QList<SchemaValidator::Error>& errors);

} // namespace PhosphorFsLoader
