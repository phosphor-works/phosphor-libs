// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorPaths/TreeMove.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>

namespace PhosphorPaths {

void moveTreeContents(const QString& from, const QString& to)
{
    if (from.isEmpty() || !QFileInfo(from).isDir()) {
        return;
    }
    const QDir source(from);
    QDirIterator files(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString file = files.next();
        const QString target = to + QLatin1Char('/') + source.relativeFilePath(file);
        if (QFileInfo::exists(target)) {
            continue;
        }
        QDir().mkpath(QFileInfo(target).absolutePath());
        if (!QFile::rename(file, target) && QFile::copy(file, target)) {
            QFile::remove(file);
        }
    }
    // Empty directories, deepest first; a directory something stayed in
    // refuses to go.
    QDirIterator dirs(from, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    QStringList found;
    while (dirs.hasNext()) {
        found.append(dirs.next());
    }
    std::sort(found.begin(), found.end(), [](const QString& a, const QString& b) {
        return a.size() > b.size();
    });
    for (const QString& dir : std::as_const(found)) {
        QDir().rmdir(dir);
    }
    QDir().rmdir(from);
}

} // namespace PhosphorPaths
