// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorWorkspaces/Plasma/KWinVirtualDesktopBackend.h>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>

#include <array>
#include <limits>
#include <memory>
#include <utility>

namespace PhosphorWorkspaces {

namespace {

QString kwinService()
{
    return QStringLiteral("org.kde.KWin");
}

QString kwinVdmPath()
{
    return QStringLiteral("/VirtualDesktopManager");
}

QString kwinVdmInterface()
{
    return QStringLiteral("org.kde.KWin.VirtualDesktopManager");
}

/// Bound on every call this class makes to KWin. Far beyond any healthy round
/// trip, and short enough that a wedged-but-registered KWin cannot stall a
/// caller or hold the one list read in flight for long.
constexpr int KWinCallTimeoutMs = 1000;

/// Read desktopCreated's or desktopDataChanged's s(iss) payload. The struct's
/// position is `u` on the wire (KWin's DBusDesktopDataStruct holds a uint,
/// whatever its introspection XML says), so it is read as one. False for a
/// message that does not carry a well-formed payload for a non-empty id.
bool parseDesktopData(const QDBusMessage& message, QString& desktopId, int& position, QString& name)
{
    const QList<QVariant> args = message.arguments();
    if (args.size() < 2 || args.at(1).userType() != qMetaTypeId<QDBusArgument>()) {
        return false;
    }
    desktopId = args.at(0).toString();
    const QDBusArgument data = args.at(1).value<QDBusArgument>();
    uint wirePosition = 0;
    QString dataId;
    data.beginStructure();
    data >> wirePosition >> dataId >> name;
    data.endStructure();
    if (desktopId.isEmpty() || dataId != desktopId
        || wirePosition > static_cast<uint>(std::numeric_limits<int>::max())) {
        return false;
    }
    position = static_cast<int>(wirePosition);
    return true;
}

/// A `u` off the wire as the int the manager counts in, saturating rather
/// than wrapping past INT_MAX.
int narrowWireCount(uint value)
{
    return static_cast<int>(qMin(value, static_cast<uint>(std::numeric_limits<int>::max())));
}

} // namespace

KWinVirtualDesktopBackend::KWinVirtualDesktopBackend(QObject* parent)
    : IVirtualDesktopBackend(parent)
{
}

KWinVirtualDesktopBackend::~KWinVirtualDesktopBackend()
{
    unsubscribe();
}

bool KWinVirtualDesktopBackend::connectToSource()
{
    if (m_interface) {
        return true;
    }
    // Owned here until KWin answered, so a failed probe is freed without a
    // manual delete and leaves m_interface null for a later attempt.
    auto kwinInterface = std::make_unique<QDBusInterface>(kwinService(), kwinVdmPath(), kwinVdmInterface(),
                                                          QDBusConnection::sessionBus());
    if (!kwinInterface->isValid()) {
        return false;
    }
    kwinInterface->setParent(this);
    m_interface = kwinInterface.release();
    // Bounds the calls made THROUGH the interface: the blocking `rows` /
    // `current` property reads in readSnapshot and the createDesktop /
    // removeDesktop / setDesktopName method calls, which then answer NoReply
    // after a second instead of QDBusInterface's 25-second default
    // (createDesktop reports that as unanswered). The list reads and
    // setCurrentDesktop go through the connection with the same bound passed
    // per call.
    //
    // This does NOT bound everything: QDBusInterface's constructor above
    // already performed a blocking Introspect call at the 25-second default
    // before isValid() could be consulted, and setTimeout cannot reach
    // backwards to it.
    m_interface->setTimeout(KWinCallTimeoutMs);
    return true;
}

bool KWinVirtualDesktopBackend::isReachable() const
{
    return m_interface && m_interface->isValid();
}

void KWinVirtualDesktopBackend::subscribe()
{
    if (m_subscribed || !m_interface) {
        return;
    }
    m_subscribed = true;

    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("currentChanged"), this,
                SLOT(onCurrentChanged(QString)));
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("countChanged"), this,
                SLOT(onCountChanged(uint)));
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("desktopCreated"), this,
                SLOT(onDesktopCreated(QDBusMessage)));
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("desktopDataChanged"), this,
                SLOT(onDesktopDataChanged(QDBusMessage)));
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("desktopRemoved"), this,
                SLOT(onDesktopRemoved(QString)));
    bus.connect(kwinService(), kwinVdmPath(), kwinVdmInterface(), QStringLiteral("rowsChanged"), this,
                SLOT(onRowsChanged(uint)));
}

void KWinVirtualDesktopBackend::unsubscribe()
{
    if (!m_subscribed) {
        return;
    }
    m_subscribed = false;
    // Each (signal, slot) pair must be named EXPLICITLY. QDBusConnection
    // registers hooks keyed on signal name plus path, and its disconnect()
    // returns false for a null slot or an empty signal name — a wildcard
    // "drop everything for this interface" call compiles, runs, and removes
    // nothing at all. Mirrors the connect() calls in subscribe().
    // Not constexpr: SLOT() expands to a qFlagLocation() call.
    const std::array subscriptions{
        std::pair<const char*, const char*>{"currentChanged", SLOT(onCurrentChanged(QString))},
        std::pair<const char*, const char*>{"countChanged", SLOT(onCountChanged(uint))},
        std::pair<const char*, const char*>{"desktopCreated", SLOT(onDesktopCreated(QDBusMessage))},
        std::pair<const char*, const char*>{"desktopDataChanged", SLOT(onDesktopDataChanged(QDBusMessage))},
        std::pair<const char*, const char*>{"desktopRemoved", SLOT(onDesktopRemoved(QString))},
        std::pair<const char*, const char*>{"rowsChanged", SLOT(onRowsChanged(uint))},
    };
    for (const auto& [signalName, slotSpec] : subscriptions) {
        QDBusConnection::sessionBus().disconnect(kwinService(), kwinVdmPath(), kwinVdmInterface(),
                                                 QLatin1String(signalName), this, slotSpec);
    }
}

QList<DesktopEntry> KWinVirtualDesktopBackend::readDesktopArray(const QDBusArgument& arg)
{
    // `position` is read as int even though the WIRE type is a(uss).
    // KWin's introspection XML advertises a(iss), but `busctl get-property`
    // shows the marshalled value is actually a(uss). Reading it into an int
    // is bit-compatible for the non-negative positions KWin emits, and
    // QDBusDemarshaller's union read yields the right value — verified
    // against the live interface rather than inferred. Flagged here because
    // the XML and the wire disagree, which is exactly the kind of thing a
    // future reader would "fix" in the wrong direction.
    QList<DesktopEntry> desktops;
    arg.beginArray();
    while (!arg.atEnd()) {
        DesktopEntry entry;
        arg.beginStructure();
        arg >> entry.position >> entry.id >> entry.name;
        arg.endStructure();
        desktops.append(entry);
    }
    arg.endArray();
    return desktops;
}

std::optional<IVirtualDesktopBackend::Snapshot> KWinVirtualDesktopBackend::readSnapshot()
{
    if (!isReachable()) {
        return std::nullopt;
    }
    Snapshot snapshot;
    // The scalar properties go into the snapshot, which the manager commits
    // together with the desktop list or not at all.
    if (const QVariant rows = m_interface->property("rows"); rows.isValid()) {
        snapshot.rows = rows.toInt();
    }
    if (const QVariant current = m_interface->property("current"); current.isValid()) {
        snapshot.currentId = current.toString();
    }

    QDBusMessage getDesktops = QDBusMessage::createMethodCall(
        kwinService(), kwinVdmPath(), QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    getDesktops << kwinVdmInterface() << QStringLiteral("desktops");
    const QDBusMessage reply = QDBusConnection::sessionBus().call(getDesktops, QDBus::Block, KWinCallTimeoutMs);

    // The Get(desktops) reply envelope: a variant around the array.
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return std::nullopt;
    }
    const QVariant inner = reply.arguments().at(0).value<QDBusVariant>().variant();
    if (inner.userType() != qMetaTypeId<QDBusArgument>()) {
        return std::nullopt;
    }
    snapshot.desktops = readDesktopArray(*static_cast<const QDBusArgument*>(inner.constData()));
    return snapshot;
}

void KWinVirtualDesktopBackend::readSnapshotAsync(SnapshotCallback done)
{
    if (!isReachable()) {
        done(std::nullopt);
        return;
    }
    // ONE async GetAll for rows + current + desktops, so the whole snapshot
    // lands in a single round trip without blocking the GUI thread.
    QDBusMessage getAll = QDBusMessage::createMethodCall(
        kwinService(), kwinVdmPath(), QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    getAll << kwinVdmInterface();

    // Parented to this backend, so the answer is never delivered after it.
    auto* watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(getAll, KWinCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [done = std::move(done)](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        QDBusPendingReply<QVariantMap> reply = *w;
        if (reply.isError()) {
            done(std::nullopt);
            return;
        }
        const QVariantMap props = reply.value();
        Snapshot snapshot;
        if (const QVariant rows = props.value(QStringLiteral("rows")); rows.isValid()) {
            snapshot.rows = rows.toInt();
        }
        snapshot.currentId = props.value(QStringLiteral("current")).toString();
        // In a GetAll a{sv} map the complex-typed `desktops` value
        // arrives as a QDBusArgument inside the QVariant; the scalar
        // members above demarshal to plain types.
        const QVariant desktops = props.value(QStringLiteral("desktops"));
        if (desktops.userType() != qMetaTypeId<QDBusArgument>()) {
            done(std::nullopt);
            return;
        }
        snapshot.desktops = readDesktopArray(*static_cast<const QDBusArgument*>(desktops.constData()));
        done(std::move(snapshot));
    });
}

void KWinVirtualDesktopBackend::setCurrentDesktop(const QString& desktopId)
{
    if (!m_interface) {
        return;
    }
    // Properties.Set, sent without waiting for the reply. QDBusInterface's
    // setProperty blocks the calling thread (the shell's GUI thread, for a
    // pager) until KWin answers; the switch itself arrives as currentChanged
    // either way (F706).
    QDBusMessage setCurrent = QDBusMessage::createMethodCall(
        kwinService(), kwinVdmPath(), QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Set"));
    setCurrent << kwinVdmInterface() << QStringLiteral("current") << QVariant::fromValue(QDBusVariant(desktopId));
    QDBusConnection::sessionBus().send(setCurrent);
}

void KWinVirtualDesktopBackend::createDesktop(uint position, const QString& name, AnswerCallback done)
{
    if (!m_interface) {
        return;
    }
    // KWin emits desktopCreated before it returns, and the signal and the
    // reply share one ordered connection, so the answer is handled after the
    // echo it would have caused.
    auto* watcher =
        new QDBusPendingCallWatcher(m_interface->asyncCall(QStringLiteral("createDesktop"), position, name), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [done = std::move(done)](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        done(!w->isError());
    });
}

void KWinVirtualDesktopBackend::removeDesktop(const QString& desktopId, AnswerCallback done)
{
    if (!m_interface) {
        return;
    }
    auto* watcher =
        new QDBusPendingCallWatcher(m_interface->asyncCall(QStringLiteral("removeDesktop"), desktopId), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [done = std::move(done)](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        done(!w->isError());
    });
}

void KWinVirtualDesktopBackend::setDesktopName(const QString& desktopId, const QString& name)
{
    if (!m_interface) {
        return;
    }
    m_interface->asyncCall(QStringLiteral("setDesktopName"), desktopId, name);
}

void KWinVirtualDesktopBackend::onCurrentChanged(const QString& desktopId)
{
    Q_EMIT currentChanged(desktopId);
}

void KWinVirtualDesktopBackend::onCountChanged(uint count)
{
    // `uint` is a wire type only: narrow once at this boundary. A count past
    // INT_MAX is no count KWin sends, so it is dropped rather than saturated.
    if (count > static_cast<uint>(std::numeric_limits<int>::max())) {
        return;
    }
    Q_EMIT countChanged(static_cast<int>(count));
}

void KWinVirtualDesktopBackend::onDesktopCreated(const QDBusMessage& message)
{
    QString desktopId;
    int position = 0;
    QString name;
    if (parseDesktopData(message, desktopId, position, name)) {
        Q_EMIT desktopCreated(desktopId, position, name);
        return;
    }
    // The id alone, when that much is readable: the create still happened.
    const QList<QVariant> args = message.arguments();
    const QString echoedId =
        (!args.isEmpty() && args.at(0).userType() == QMetaType::QString) ? args.at(0).toString() : QString();
    Q_EMIT desktopCreatedUnreadable(echoedId);
}

void KWinVirtualDesktopBackend::onDesktopRemoved(const QString& desktopId)
{
    Q_EMIT desktopRemoved(desktopId);
}

void KWinVirtualDesktopBackend::onRowsChanged(uint rows)
{
    Q_EMIT rowsChanged(narrowWireCount(rows));
}

void KWinVirtualDesktopBackend::onDesktopDataChanged(const QDBusMessage& message)
{
    QString desktopId;
    int position = 0;
    QString name;
    if (parseDesktopData(message, desktopId, position, name)) {
        Q_EMIT desktopDataChanged(desktopId, position, name);
        return;
    }
    Q_EMIT desktopDataChangedUnreadable();
}

} // namespace PhosphorWorkspaces
