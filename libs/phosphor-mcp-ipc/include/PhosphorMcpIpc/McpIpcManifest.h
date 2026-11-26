// SPDX-FileCopyrightText: 2026 Phosphor Works Team
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
    // The target and the method of "call": "TARGET.METHOD", split at the
    // last dot, since a method name has none.
    QString target;
    QString method;
    QString name;
    QString title;
    QString description;
    // The parameter names of the overload, in order. An empty list selects
    // the overload without parameters. Absent selects the method's only
    // overload.
    std::optional<QStringList> params;
    // The behavior hints of "annotations", each absent when not given. The
    // server sends only the hints that are given, and a client assumes the
    // least safe value for one that it does not get.
    std::optional<bool> readOnly;
    std::optional<bool> destructive;
    std::optional<bool> idempotent;
    std::optional<bool> openWorld;
};

// The manifest that an application installs: the calls that an agent may
// make, each as a tool. Nothing else of the application is offered.
//
//     {"tools": [{"call": "TARGET.METHOD", "name": "tool_name", "title": "optional",
//                 "description": "for the model", "params": ["a", "b"],
//                 "annotations": {"readOnly": true, "destructive": false,
//                                 "idempotent": true, "openWorld": false}}]}
//
// The shape is strict, so that a typo does not drop a restriction without
// a word. Each of these is an error:
//   - a member that the shape does not name, at the root, in a tool or in
//     "annotations";
//   - a member of the wrong type;
//   - no "call", or one that is not TARGET.METHOD with a target and a
//     method;
//   - no "name", one that McpTool::isValidName() refuses, or one that two
//     tools use;
//   - an empty "title" or "description", or no "description";
//   - "params" that holds an empty name, or one name two times.
// JSON itself takes the last of two members of one name in an object. The
// parser does not see the first.
struct PHOSPHORMCPIPC_EXPORT McpIpcManifest
{
    QList<McpIpcManifestTool> tools;

    // What is wrong with the tools, as parse() reports it, or nothing. For
    // a manifest that code built without parse(). McpIpcBridge checks it.
    [[nodiscard]] QString problem() const;

    // The manifest in `json`, or nothing with the reason in `error`.
    // `error` may be null. It is cleared when the manifest is read.
    [[nodiscard]] static std::optional<McpIpcManifest> parse(const QByteArray& json, QString* error = nullptr);
    // The manifest in the file at `path`. No file at the path is a
    // manifest without tools, and `missing` (when given) is set. Each of
    // these is an error:
    //   - a file that cannot be read or parsed
    //   - a link to nothing
    //   - a path that is no regular file (a directory, a pipe, a device)
    //   - a file larger than 4 MiB
    // `error` may be null. It is cleared when the manifest is read.
    [[nodiscard]] static std::optional<McpIpcManifest> load(const QString& path, QString* error = nullptr,
                                                            bool* missing = nullptr);
};

} // namespace PhosphorMcpIpc
