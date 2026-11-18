// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorworkspaces_export.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace PhosphorWorkspaces {

/// One desktop of a compositor's list: 0-based position, stable id, raw name.
struct DesktopEntry
{
    int position = 0;
    QString id;
    QString name;
};

/**
 * @brief Where a VirtualDesktopManager's desktops come from: the transport to
 *        the compositor that owns them.
 *
 * The manager keeps every rule about the desktops (validating a list, the list
 * built from the change signals, renumbering, the per-screen desktops, one read
 * in flight, retrying a failed read). A backend only reads, writes and reports
 * what the compositor says, in the order the compositor says it.
 *
 * KWin's backend talks to org.kde.KWin.VirtualDesktopManager on the session bus
 * (PhosphorWorkspaces::Plasma). A compositor that owns its desktops in process
 * implements this directly.
 *
 * Signals go out only between subscribe() and unsubscribe(), and in the order
 * the compositor produced the changes. A read answers with the compositor's
 * state after every change signal emitted before the answer.
 */
class PHOSPHORWORKSPACES_EXPORT IVirtualDesktopBackend : public QObject
{
    Q_OBJECT

public:
    /// One whole desktop list, with the current desktop and the grid's rows.
    struct Snapshot
    {
        QList<DesktopEntry> desktops;
        /// The current desktop's id, empty when the compositor did not say.
        QString currentId;
        /// The grid's row count, nullopt when the compositor did not say.
        std::optional<int> rows;
    };
    using SnapshotCallback = std::function<void(std::optional<Snapshot> snapshot)>;
    /// @p answered is false for an error or a timeout: a stall, not a refusal.
    using AnswerCallback = std::function<void(bool answered)>;

    explicit IVirtualDesktopBackend(QObject* parent = nullptr)
        : QObject(parent)
    {
    }
    ~IVirtualDesktopBackend() override = default;

    /// Reach the compositor's desktops. False when there are none to reach;
    /// the manager then stays unavailable. Called once.
    virtual bool connectToSource() = 0;
    /// Whether a read can reach the desktops right now. KWin's backend says no
    /// while KWin is off the bus (a restart). The manager reads nothing then,
    /// and commits a count change as reported instead of reading the list.
    virtual bool isReachable() const = 0;
    /// Start and stop emitting the change signals. Each is called only when
    /// the other was last, so neither needs to guard against doubling.
    virtual void subscribe() = 0;
    virtual void unsubscribe() = 0;

    /// Read the whole list now, blocking the caller. nullopt when the read
    /// failed. Used only while the manager is not running (before start(),
    /// after stop()).
    virtual std::optional<Snapshot> readSnapshot() = 0;
    /// Read the whole list without blocking. @p done runs once, with nullopt
    /// when the read failed, and never after this backend is destroyed.
    virtual void readSnapshotAsync(SnapshotCallback done) = 0;

    /// Switch to the desktop with this id, without waiting for the switch: it
    /// arrives as currentChanged.
    virtual void setCurrentDesktop(const QString& desktopId) = 0;
    /// Create a desktop at a 0-based position. @p done runs once the
    /// compositor answered, after any desktopCreated the call caused, and
    /// never after this backend is destroyed.
    virtual void createDesktop(uint position, const QString& name, AnswerCallback done) = 0;
    /// Remove a desktop. @p done as for createDesktop.
    virtual void removeDesktop(const QString& desktopId, AnswerCallback done) = 0;
    /// Rename a desktop. The new name arrives as desktopDataChanged.
    virtual void setDesktopName(const QString& desktopId, const QString& name) = 0;

Q_SIGNALS:
    void currentChanged(const QString& desktopId);
    /// The desktop count, which closes a create, a remove or a resize: every
    /// desktop signal of the change went out before it.
    void countChanged(int count);
    void desktopCreated(const QString& desktopId, int position, const QString& name);
    /// A desktop was created but its data could not be read. @p desktopId is
    /// the id when that much was readable, else empty.
    void desktopCreatedUnreadable(const QString& desktopId);
    /// A desktop was renamed or moved; @p position is its new one.
    void desktopDataChanged(const QString& desktopId, int position, const QString& name);
    /// A desktop's data changed but could not be read.
    void desktopDataChangedUnreadable();
    void desktopRemoved(const QString& desktopId);
    /// The grid's row count changed (a reshape moves no desktop).
    void rowsChanged(int rows);
};

} // namespace PhosphorWorkspaces
