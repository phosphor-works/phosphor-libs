// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorSurface/SurfacePackLocation.h>

#include <QMutex>
#include <QMutexLocker>

namespace PhosphorSurfaceShaders {

namespace {

// One per process. The mutex is for the readers: the include-path list is
// built on the GUI thread, and a builtin buffer token can be resolved from a
// registry scan on another.
struct Location
{
    QMutex mutex;
    QString subdir;
};

Location& location()
{
    static Location instance;
    return instance;
}

} // namespace

void setSurfacePackDataSubdir(const QString& subdir)
{
    Location& loc = location();
    const QMutexLocker lock(&loc.mutex);
    loc.subdir = subdir;
}

QString surfacePackDataSubdir()
{
    Location& loc = location();
    const QMutexLocker lock(&loc.mutex);
    return loc.subdir;
}

} // namespace PhosphorSurfaceShaders
