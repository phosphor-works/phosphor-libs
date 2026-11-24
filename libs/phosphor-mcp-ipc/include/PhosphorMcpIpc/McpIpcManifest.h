// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorMcpIpc/phosphormcpipc_export.h>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace PhosphorMcpIpc {

// One tool of the manifest: the phosphor-ipc call that it makes, and what
// tools/list shows of it.
struct McpIpcManifestTool
{
    // The target and the function of "call": "TARGET.METHOD", split at the
    // last dot, since a function name has none.
    QString target;
    QString function;
    QString name;
    QString title;
    QString description;
    // The parameter names of the overload, in order. Absent selects the
    // function's only overload.
    std::optional<QStringList> params;
    // The behavior hints of "annotations", each absent when not given.
    std::optional<bool> readOnly;
    std::optional<bool> destructive;
    std::optional<bool> idempotent;
    std::optional<bool> openWorld;
};

// The manifest that an application installs beside its socket: the calls
// that an agent may make, each as a tool. Nothing else of the application
// is offered.
//
//     {"tools": [{"call": "TARGET.METHOD", "name": "tool_name", "title": "optional",
//                 "description": "for the model", "params": ["a", "b"],
//                 "annotations": {"readOnly": true, "destructive": false,
//                                 "idempotent": true, "openWorld": false}}]}
//
// The shape is strict: a member that it does not name, a member of the
// wrong type, and a tool without call, name or description are errors, so
// that a typo does not drop a restriction without a word.
struct PHOSPHORMCPIPC_EXPORT McpIpcManifest
{
    QList<McpIpcManifestTool> tools;

    // The manifest in `json`, or nothing with the reason in `error`.
    [[nodiscard]] static std::optional<McpIpcManifest> parse(const QByteArray& json, QString* error);
    // The manifest in the file at `path`. A file that does not exist is a
    // manifest without tools, and `missing` (when given) is set. A file that
    // cannot be read or parsed is an error.
    [[nodiscard]] static std::optional<McpIpcManifest> load(const QString& path, QString* error,
                                                            bool* missing = nullptr);
};

} // namespace PhosphorMcpIpc
