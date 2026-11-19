// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorZoneRendering/OverlayPackLocation.h>

#include <QMutex>
#include <QMutexLocker>

namespace PhosphorZoneRendering {

namespace {

// One per process. The mutex is for the readers: a ZoneShaderItem resolves its
// include paths on the GUI thread, and a warm bake can run on another.
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

void setOverlayPackDataSubdir(const QString& subdir)
{
    Location& loc = location();
    const QMutexLocker lock(&loc.mutex);
    loc.subdir = subdir;
}

QString overlayPackDataSubdir()
{
    Location& loc = location();
    const QMutexLocker lock(&loc.mutex);
    return loc.subdir;
}

} // namespace PhosphorZoneRendering
