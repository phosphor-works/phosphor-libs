// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorWorkspaces/IVirtualDesktopBackend.h>
#include <PhosphorWorkspaces/Plasma/phosphorworkspacesplasma_export.h>

QT_BEGIN_NAMESPACE
class QDBusArgument;
class QDBusInterface;
class QDBusMessage;
QT_END_NAMESPACE

namespace PhosphorWorkspaces {

/**
 * @brief KWin's virtual desktops, over org.kde.KWin.VirtualDesktopManager on
 *        the session bus.
 *
 * Every call to KWin is bounded at a second. The list reads go through the
 * connection KWin sends its desktop signals on, so a read's reply comes after
 * every signal KWin sent before handling it.
 */
class PHOSPHORWORKSPACESPLASMA_EXPORT KWinVirtualDesktopBackend : public IVirtualDesktopBackend
{
    Q_OBJECT

public:
    explicit KWinVirtualDesktopBackend(QObject* parent = nullptr);
    ~KWinVirtualDesktopBackend() override;

    /// Probe KWin's interface. False on a session without KWin.
    bool connectToSource() override;
    /// KWin's service is on the bus (QDBusInterface follows its owner).
    bool isReachable() const override;
    void subscribe() override;
    void unsubscribe() override;
    std::optional<Snapshot> readSnapshot() override;
    void readSnapshotAsync(SnapshotCallback done) override;
    void setCurrentDesktop(const QString& desktopId) override;
    void createDesktop(uint position, const QString& name, AnswerCallback done) override;
    void removeDesktop(const QString& desktopId, AnswerCallback done) override;
    void setDesktopName(const QString& desktopId, const QString& name) override;

private Q_SLOTS:
    // KWin's signals off the bus. The parameter types are the wire types:
    // QDBusConnection::connect matches a slot against the message's signature,
    // so a slot declared `int` for KWin's `u` registers successfully and is
    // then NEVER invoked.
    void onCurrentChanged(const QString& desktopId);
    void onCountChanged(uint count);
    /// desktopCreated is s(iss); the whole message is taken so the struct can
    /// be read.
    void onDesktopCreated(const QDBusMessage& message);
    void onDesktopRemoved(const QString& desktopId);
    void onRowsChanged(uint rows);
    /// A rename or a position move, as s(iss).
    void onDesktopDataChanged(const QDBusMessage& message);

private:
    /// Parse the a(uss) desktop array. A malformed one demarshals to entries
    /// the manager's commit refuses (an empty id).
    static QList<DesktopEntry> readDesktopArray(const QDBusArgument& arg);

    QDBusInterface* m_interface = nullptr;
    bool m_subscribed = false;
};

} // namespace PhosphorWorkspaces
