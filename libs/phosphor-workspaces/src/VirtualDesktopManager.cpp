// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorWorkspaces/VirtualDesktopManager.h>

#include <PhosphorIdentity/VirtualScreenId.h>

#include <QSet>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

namespace PhosphorWorkspaces {

namespace {

/// Placeholder label for a desktop KWin gave no name to.
///
/// Deliberately untranslated. `PhosphorI18n::tr()` — the project's only
/// sanctioned translation entry point for C++ — lives in `plasmazones/src/phosphor_i18n.h`,
/// inside the GPL-3.0 application tree. This library is LGPL-2.1 precisely so
/// third-party tools can link it without inheriting GPL, so reaching into that
/// header would defeat the licence split the project maintains on purpose.
/// The shell tier that renders these labels has no translation wiring either,
/// so the string localises when that story lands, together with the rest of
/// the shell's text. Same reasoning as the bar's widget display names.
QString fallbackDesktopName(int oneBasedPosition)
{
    return QStringLiteral("Desktop %1").arg(oneBasedPosition);
}

/// A failed list read is retried this many times, this far apart, before the
/// change it was reading is given up (retryOrAbandonRefresh).
constexpr int MaxRefreshRetries = 3;
constexpr int RefreshRetryDelayMs = 500;

} // namespace

VirtualDesktopManager::VirtualDesktopManager(QObject* parent)
    : VirtualDesktopManager(nullptr, parent)
{
}

VirtualDesktopManager::VirtualDesktopManager(std::unique_ptr<IVirtualDesktopBackend> backend, QObject* parent)
    : QObject(parent)
    , m_backend(std::move(backend))
{
    if (m_backend) {
        // Owned through m_backend, so no Qt parent to delete it a second time.
        m_backend->setParent(nullptr);
    }
}

VirtualDesktopManager::~VirtualDesktopManager()
{
    stop();
}

bool VirtualDesktopManager::init()
{
    // Re-entry guard: a second init() would connect to the source again and
    // double every subscription, so each change signal would be handled twice
    // while stop() removed only one of each.
    if (m_available || !m_backend) {
        return true;
    }
    if (!m_backend->connectToSource()) {
        return true;
    }
    m_available = true;
    refreshFromSource();
    subscribeToSource();
    return true;
}

void VirtualDesktopManager::subscribeToSource()
{
    // Guarded so a stop()/start() cycle re-subscribes exactly once; a doubled
    // subscription would handle every change signal twice while stop()
    // removed only one of each.
    if (m_subscribed || !m_available) {
        return;
    }
    m_subscribed = true;

    IVirtualDesktopBackend* const source = m_backend.get();
    m_sourceConnections = {
        connect(source, &IVirtualDesktopBackend::currentChanged, this, &VirtualDesktopManager::onSourceCurrentChanged),
        connect(source, &IVirtualDesktopBackend::countChanged, this, &VirtualDesktopManager::onSourceCountChanged),
        connect(source, &IVirtualDesktopBackend::desktopCreated, this,
                [this](const QString& desktopId, int position, const QString& name) {
                    ingestDesktopCreated(desktopId, position, name);
                }),
        connect(source, &IVirtualDesktopBackend::desktopCreatedUnreadable, this,
                &VirtualDesktopManager::onSourceDesktopCreatedUnreadable),
        // Renames and position moves. The count stays untouched for these, so
        // without this a stale desktop name would survive until an unrelated
        // create/remove forced a refresh.
        connect(source, &IVirtualDesktopBackend::desktopDataChanged, this,
                [this](const QString& desktopId, int position, const QString& name) {
                    ingestDesktopDataChanged(desktopId, position, name);
                }),
        connect(source, &IVirtualDesktopBackend::desktopDataChangedUnreadable, this,
                &VirtualDesktopManager::refreshFromSource),
        connect(source, &IVirtualDesktopBackend::desktopRemoved, this, &VirtualDesktopManager::onSourceDesktopRemoved),
        // A live grid reshape (e.g. 1×4 → 2×2) changes the rows WITHOUT
        // changing the desktop count, so it fires neither countChanged nor
        // created/removed — without this the cached row count goes stale and
        // cross-desktop directional navigation computes neighbours against the
        // wrong grid shape.
        connect(source, &IVirtualDesktopBackend::rowsChanged, this, &VirtualDesktopManager::onSourceRowsChanged),
    };
    m_backend->subscribe();
}

void VirtualDesktopManager::unsubscribeFromSource()
{
    if (!m_subscribed) {
        return;
    }
    m_subscribed = false;
    m_backend->unsubscribe();
    for (const QMetaObject::Connection& connection : std::as_const(m_sourceConnections)) {
        disconnect(connection);
    }
    m_sourceConnections.clear();
}

bool VirtualDesktopManager::commitSnapshot(IVirtualDesktopBackend::Snapshot snapshot)
{
    // Rows clamp to >= 1 so a zero cannot divide the grid arithmetic by zero;
    // a snapshot that did not say keeps the rows this manager has.
    const int rows = snapshot.rows ? qMax(1, *snapshot.rows) : m_desktopRows;
    return commitDesktopList(std::move(snapshot.desktops), snapshot.currentId, rows);
}

bool VirtualDesktopManager::commitDesktopList(QList<DesktopEntry> desktops, const QString& currentId, int rows)
{
    // Validate before touching a member, and commit nothing on failure. The
    // members must not be cleared up front: a failed reply (KWin restarting,
    // a congested bus, an async call that errored) would then leave ids empty
    // while the count kept its previous value, and the "ids and names are
    // index-aligned" contract this class publishes would be broken. Every
    // id-keyed operation fails silently in that state: the pager empties, and
    // both setCurrentDesktopById and setCurrentDesktop drop every switch
    // because they validate against the empty id list.
    //
    // An empty list is well-formed, but a compositor always has at least one
    // desktop, so it means the backend read nothing usable. A backend's reads
    // are a system boundary (KWin's D-Bus replies demarshal to DEFAULTS
    // silently on a signature mismatch), so an empty id (never a real KWin
    // UUID) or a repeated one rejects the whole list too.
    if (desktops.isEmpty()) {
        return false;
    }
    QSet<QString> seen;
    seen.reserve(desktops.size());
    for (const auto& desktop : std::as_const(desktops)) {
        if (desktop.id.isEmpty() || seen.contains(desktop.id)) {
            return false;
        }
        seen.insert(desktop.id);
    }

    // Stable, so two entries reporting the same position keep the order KWin
    // listed them in.
    std::stable_sort(desktops.begin(), desktops.end(), [](const DesktopEntry& a, const DesktopEntry& b) {
        return a.position < b.position;
    });

    QStringList newIds;
    QStringList newNames;
    QStringList newRawNames;
    newIds.reserve(desktops.size());
    newNames.reserve(desktops.size());
    newRawNames.reserve(desktops.size());
    for (const auto& desktop : std::as_const(desktops)) {
        newIds.append(desktop.id);
        newRawNames.append(desktop.name);
        newNames.append(desktop.name.isEmpty() ? fallbackDesktopName(newIds.size()) : desktop.name);
    }

    // The count IS the list length. Reading it from the separate `count`
    // property would reintroduce the possibility of the two disagreeing.
    const int newCount = newIds.size();

    // How the POSITIONS moved, while the old list is still here to compare
    // against. Ids are stable across a renumber and positions are not, so this
    // is the only moment the answer exists — see desktopRemovedAt's doc for
    // why the count alone cannot supply it. One map for every shape of change
    // (F787). The first snapshot has no previous list and compares against the
    // previous session's, when the daemon handed one over.
    const bool liveChange = !m_desktopIds.isEmpty();
    const PhosphorEngine::DesktopRemap remap =
        PhosphorEngine::DesktopRemap::fromIds(liveChange ? m_desktopIds : m_baselineDesktopIds, newIds);
    m_baselineDesktopIds.clear();
    const int removedPosition = liveChange ? remap.singleRemoval() : 0;

    // The current desktop by its id. An id this list lacks (the current
    // desktop was removed and no currentChanged named its replacement) goes
    // where KWin sends a removed desktop's screens, its successor. Clamped
    // either way: a removal BEFORE the current one renumbers it downward
    // without KWin sending currentChanged (the id did not move), so this is
    // the only place an out-of-range position is guaranteed to be caught.
    int newCurrent = m_currentDesktop;
    const int currentIdx = currentId.isEmpty() ? -1 : newIds.indexOf(currentId);
    if (currentIdx >= 0) {
        newCurrent = currentIdx + 1;
    } else if (liveChange && !remap.isIdentity()) {
        newCurrent = remap.successor(m_currentDesktop);
    }
    newCurrent = qBound(1, newCurrent, qMax(1, newCount));

    const bool listChanged = (newIds != m_desktopIds || newNames != m_desktopNames || newRawNames != m_rawDesktopNames);
    const bool idsChanged = (newIds != m_desktopIds);
    const bool currentChanged = (newCurrent != m_currentDesktop);
    const bool countChanged = (newCount != m_desktopCount);

    // Renames: a desktop on both sides of the commit whose raw name moved.
    QList<std::pair<QString, QString>> renamed;
    if (liveChange) {
        for (int i = 0; i < newIds.size(); ++i) {
            const int oldIdx = m_desktopIds.indexOf(newIds.at(i));
            if (oldIdx >= 0 && oldIdx < m_rawDesktopNames.size() && m_rawDesktopNames.at(oldIdx) != newRawNames.at(i)) {
                renamed.append({newIds.at(i), newRawNames.at(i)});
            }
        }
    }

    // This commit ends the change: the next KWin signal is a new one.
    const bool changeWasPending = m_desktopListChangePending;
    m_desktopListChangePending = false;
    m_pendingCurrentId.clear();

    m_desktopIds = newIds;
    m_desktopNames = newNames;
    m_rawDesktopNames = newRawNames;
    m_desktopCount = newCount;
    m_desktopRows = qMax(1, rows);
    m_currentDesktop = newCurrent;
    m_liveIds = newIds;
    m_liveRawNames.clear();
    for (int i = 0; i < newIds.size(); ++i) {
        m_liveRawNames.insert(newIds.at(i), newRawNames.at(i));
    }

    // This manager's OWN per-screen mirror moves through the same map, and
    // BEFORE any signal goes out: a consumer of desktopListChanged reads the
    // per-screen desktops against the new list (F111). A screen showing a
    // removed desktop takes its successor, as KWin moves it there, and
    // anything past the new count is pulled down with it, so
    // clampScreenDesktopsToCount below finds nothing to correct.
    //
    // SILENTLY, with no screenDesktopChanged, and the reason is worth keeping:
    // that signal means "this output switched desktops" and its subscriber
    // runs the whole context-switch pass — cancelling drag previews,
    // re-pinning sticky screens, pushing the desktop into all three engines,
    // re-announcing the screen set and showing the switch OSD. Emitting it
    // here would run that pass against engine state that has NOT been
    // renumbered yet, since the renumber signals go out below, and the push it
    // makes would then be moved a SECOND time by the engines' own renumber.
    // Nothing switched desktops; the desktops were renumbered underneath
    // everyone, which is what those signals say.
    //
    // A first snapshot moves no screen: the map then compares against the
    // previous session, and every screen this manager knows of was reported
    // in the live numbering.
    if (liveChange && !remap.isIdentity()) {
        for (auto it = m_screenDesktops.begin(); it != m_screenDesktops.end(); ++it) {
            it.value() = qBound(1, remap.successor(it.value()), m_desktopCount);
        }
    }

    // The settled id list FIRST, ahead of every signal below that carries a
    // desktop NUMBER (currentDesktopChanged, desktopsRenumbered, the count and
    // anything their subscribers re-report). The dynamic-workspaces reconciler
    // resolves numbers against the id list it last settled: announcing the
    // list after them would have it resolve each against the previous list.
    if (idsChanged) {
        Q_EMIT desktopListChanged(m_desktopIds);
    }
    // Change-gated: a refresh runs on create, remove, and every rename, and
    // most of those leave this data untouched. An unconditional emit would
    // reset a pager's model on unrelated events.
    if (listChanged) {
        Q_EMIT desktopsChanged();
    }
    for (const auto& [desktopId, name] : std::as_const(renamed)) {
        Q_EMIT desktopRenamed(desktopId, name);
    }
    // A desktop added or removed BEFORE the current one renumbers the current
    // position while its id stays put, so KWin sends no currentChanged and
    // this is the only place the move is observable. Consumers key placement
    // state on the 1-based number and re-read only on this signal.
    if (currentChanged) {
        Q_EMIT currentDesktopChanged(m_currentDesktop);
    }
    // BEFORE the count, deliberately. A consumer keyed on positions has to
    // drop the removed position's state and shift the rest down before
    // anything reasons about the new count — and once it has, the count
    // handler's "prune everything past newCount" sweep correctly finds
    // nothing left to do.
    if (!remap.isIdentity()) {
        Q_EMIT desktopsRenumbered(remap);
        if (removedPosition > 0) {
            Q_EMIT desktopRemovedAt(removedPosition);
        }
    }
    // The count notification belongs HERE, where the value is committed. The
    // one count committed ahead of the list is a growing one from the
    // list-less fallback in onNumberOfDesktopsChanged, and this finds it
    // already equal.
    if (countChanged) {
        clampScreenDesktopsToCount();
        Q_EMIT desktopCountChanged(m_desktopCount);
    }
    if (changeWasPending) {
        Q_EMIT desktopListChangeSettled();
    }
    return true;
}

void VirtualDesktopManager::refreshFromSource()
{
    if (!m_available || !m_backend->isReachable()) {
        return;
    }

    if (!m_running) {
        // Bump so an asynchronous read still outstanding from before a stop()
        // cannot land its older list on top of this one, and forget it.
        ++m_refreshGeneration;
        m_refreshInFlight = false;
        // The blocking read (before start() or after stop()) commits the
        // rows and the current desktop together with the desktop list, or
        // commits nothing at all: committing a fresh count beside stale ids on
        // a dropped read would break their index alignment.
        if (std::optional<IVirtualDesktopBackend::Snapshot> snapshot = m_backend->readSnapshot()) {
            commitSnapshot(std::move(*snapshot));
        }
        return;
    }

    // One read at a time (F914). The backend answers a read with the
    // compositor after every change signal it emitted before the answer
    // (KWin sends its desktop signals and its reply on one connection, in the
    // order it produced them, and handles the read whole between two
    // changes). A signal arriving while a read is in flight is therefore
    // covered by it, and a second read would only repeat it; a removal alone
    // sends one desktopDataChanged per shifted desktop plus desktopRemoved and
    // countChanged, each of which used to issue its own read.
    if (m_refreshInFlight) {
        return;
    }
    m_refreshInFlight = true;
    ++m_refreshGeneration;
    const uint thisGeneration = m_refreshGeneration;

    m_backend->readSnapshotAsync([this, thisGeneration](std::optional<IVirtualDesktopBackend::Snapshot> snapshot) {
        if (thisGeneration != m_refreshGeneration) {
            return;
        }
        m_refreshInFlight = false;
        if (!snapshot || !commitSnapshot(std::move(*snapshot))) {
            retryOrAbandonRefresh();
            return;
        }
        m_refreshRetries = 0;
    });
}

void VirtualDesktopManager::refresh()
{
    refreshFromSource();
}

void VirtualDesktopManager::retryOrAbandonRefresh()
{
    if (m_refreshRetries < MaxRefreshRetries) {
        ++m_refreshRetries;
        QTimer::singleShot(RefreshRetryDelayMs, this, [this]() {
            if (m_running) {
                refreshFromSource();
            }
        });
        return;
    }
    m_refreshRetries = 0;
    // The compositor answered nothing usable several times over. Give the change up
    // rather than hold it open: a count committed ahead of its list (the
    // fallback in onNumberOfDesktopsChanged) goes back to the list this
    // manager actually has, so desktopCount() and desktopIds() agree again,
    // and a pending change is settled so the stores that marked their entries
    // for it stop waiting. The next change signal starts over.
    m_liveIds = m_desktopIds;
    m_pendingCurrentId.clear();
    if (!m_desktopIds.isEmpty() && m_desktopCount != m_desktopIds.size()) {
        m_desktopCount = m_desktopIds.size();
        if (m_currentDesktop > m_desktopCount) {
            m_currentDesktop = m_desktopCount;
            Q_EMIT currentDesktopChanged(m_currentDesktop);
        }
        clampScreenDesktopsToCount();
        Q_EMIT desktopCountChanged(m_desktopCount);
    }
    if (m_desktopListChangePending) {
        m_desktopListChangePending = false;
        Q_EMIT desktopListChangeSettled();
    }
}

void VirtualDesktopManager::onSourceCurrentChanged(const QString& desktopId)
{
    // While a change is pending this manager's list is the OLD one and
    // KWin's id is already numbered by the new one (a removal moves the active
    // output off the removed desktop mid-change). Resolving it now would
    // publish the id's old position. The commit that ends the change numbers
    // it instead, and the read in flight (or this one) carries `current` too
    // (F950).
    if (m_desktopListChangePending) {
        m_pendingCurrentId = desktopId;
        refreshFromSource();
        return;
    }
    const int idx = m_desktopIds.indexOf(desktopId);
    if (idx < 0) {
        // An id this snapshot has not seen yet. Publishing a fallback of 1
        // would hand every consumer a wrong position until the refresh
        // corrected it — daemon placement keys on this number. Re-read
        // instead; the commit reads `current` too, so nothing is lost.
        refreshFromSource();
        return;
    }
    const int newDesktop = idx + 1;

    if (m_currentDesktop == newDesktop) {
        return;
    }

    m_currentDesktop = newDesktop;
    Q_EMIT currentDesktopChanged(m_currentDesktop);
}

void VirtualDesktopManager::onSourceDesktopCreatedUnreadable(const QString& desktopId)
{
    // Still a change: re-read the list rather than lose it. The echo goes out
    // whenever the id itself was readable, position and name unknown. A
    // compositor emits the creation before it answers the createDesktop call,
    // so an answer with no echo ahead of it reads as a refusal (KWin at its
    // ceiling), and a payload the backend could not read is no such refusal.
    // The live list is left alone without the position, so the read below,
    // not countChanged, commits this change.
    if (!desktopId.isEmpty()) {
        noteDesktopListChanging();
        Q_EMIT kwinDesktopCreated(desktopId);
    }
    refreshFromSource();
}

void VirtualDesktopManager::ingestDesktopCreated(const QString& desktopId, int position, const QString& name)
{
    // The live list before the echo: the reconciler matches its ledger on the
    // echo and settles on desktopListChanged, which this change's countChanged
    // brings (onNumberOfDesktopsChanged).
    noteDesktopListChanging();
    if (!m_liveIds.isEmpty()) {
        m_liveIds.removeAll(desktopId);
        m_liveIds.insert(qBound(0, position, int(m_liveIds.size())), desktopId);
        m_liveRawNames.insert(desktopId, name);
    }
    Q_EMIT kwinDesktopCreated(desktopId);
    refreshFromSource();
}

void VirtualDesktopManager::onSourceDesktopRemoved(const QString& desktopId)
{
    noteDesktopListChanging();
    m_liveIds.removeAll(desktopId);
    m_liveRawNames.remove(desktopId);
    Q_EMIT kwinDesktopRemoved(desktopId);
    refreshFromSource();
}

void VirtualDesktopManager::onSourceRowsChanged(int rows)
{
    // The signal carries the new row count, so no list read is needed: the
    // count is untouched by a reshape, and desktopRows() is pulled on demand.
    m_desktopRows = qMax(1, rows);
}

void VirtualDesktopManager::ingestDesktopDataChanged(const QString& desktopId, int position, const QString& name)
{
    // KWin sends this for a rename and for every desktop a create, a remove
    // or a move shifts, each carrying the desktop's NEW position. Moving the
    // desktop to that position in the live list, in the order the backend
    // emits them, ends at the compositor's list once the change is through:
    // the shifts go out
    // in ascending position, so each lands its desktop where the next one
    // cannot displace it. A rename leaves the position as it is and starts no
    // change: it renumbers nothing, and the read below commits the name.
    // Without a trusted live list (after stop()) any of these may be a move.
    if (m_liveIds.isEmpty()) {
        noteDesktopListChanging();
    } else {
        m_liveRawNames.insert(desktopId, name);
        const int at = m_liveIds.indexOf(desktopId);
        if (at != position) {
            noteDesktopListChanging();
            if (at >= 0) {
                m_liveIds.removeAt(at);
            }
            m_liveIds.insert(qBound(0, position, int(m_liveIds.size())), desktopId);
        }
    }
    refreshFromSource();
}

void VirtualDesktopManager::start()
{
    if (m_running) {
        return;
    }

    m_running = true;

    if (m_available) {
        // Re-subscribe after a prior stop(). subscribeToSource is guarded, so
        // on first start (where init() already subscribed) this is a no-op
        // rather than a doubling.
        subscribeToSource();
        refreshFromSource();
    }
}

void VirtualDesktopManager::stop()
{
    m_running = false;
    // Unsubscribe too. Clearing the flag alone left a "stopped" manager still
    // servicing every change signal, and each of those re-enters
    // refreshFromSource and issues a blocking read — so stop() did not stop
    // anything, it only changed which code path the work took. It matters
    // most during destruction: ~VirtualDesktopManager calls stop(), and a
    // signal dispatched between there and ~QObject would re-enter a
    // half-destroyed object.
    unsubscribeFromSource();
    // Signals stop arriving here, so the list built from them can no longer
    // be trusted to follow the compositor: nothing is committed from it until
    // a read has committed the compositor's own list again (start()).
    m_liveIds.clear();
    m_liveRawNames.clear();
}

int VirtualDesktopManager::currentDesktop() const
{
    return m_available ? m_currentDesktop : 1;
}

int VirtualDesktopManager::currentDesktopForScreen(const QString& screenId) const
{
    const auto it = m_screenDesktops.constFind(screenId);
    if (it != m_screenDesktops.constEnd()) {
        return it.value();
    }

    // The map is keyed by PHYSICAL output id (updateScreenDesktop is fed from
    // the effect's per-output report), but most callers ask with an EFFECTIVE
    // id, and a subdivided output only ever produces its `/vs:N` children. An
    // exact-key miss on a virtual id is therefore the normal case there, not an
    // unknown screen — resolve it against the parent output before falling back
    // to the global desktop, which would otherwise be wrong on every virtual
    // screen of a monitor running its own desktop.
    if (PhosphorIdentity::VirtualScreenId::isVirtual(screenId)) {
        const auto physical =
            m_screenDesktops.constFind(PhosphorIdentity::VirtualScreenId::extractPhysicalId(screenId));
        if (physical != m_screenDesktops.constEnd()) {
            return physical.value();
        }
    }

    return currentDesktop();
}

bool VirtualDesktopManager::perScreenModeActive() const
{
    if (m_screenDesktops.size() < 2) {
        return false;
    }
    auto it = m_screenDesktops.constBegin();
    const int first = it.value();
    for (++it; it != m_screenDesktops.constEnd(); ++it) {
        if (it.value() != first) {
            return true;
        }
    }
    return false;
}

QString VirtualDesktopManager::liveScreenIdFor(const QString& screenId) const
{
    return m_renamedScreenIds.value(screenId, screenId);
}

QString VirtualDesktopManager::takeReportedScreenId(const QString& screenId)
{
    if (const auto alias = m_renamedScreenIds.constFind(screenId); alias != m_renamedScreenIds.constEnd()) {
        return alias.value();
    }
    m_renamedScreenIds.removeIf([&screenId](QHash<QString, QString>::iterator alias) {
        return alias.value() == screenId;
    });
    return screenId;
}

void VirtualDesktopManager::updateScreenDesktop(const QString& reportedScreenId, int desktop)
{
    if (reportedScreenId.isEmpty() || desktop < 1) {
        return;
    }
    const QString screenId = takeReportedScreenId(reportedScreenId);
    if (m_screenDesktops.value(screenId, -1) == desktop) {
        return;
    }
    m_screenDesktops.insert(screenId, desktop);
    Q_EMIT screenDesktopChanged(screenId, desktop);
}

void VirtualDesktopManager::removeScreenDesktop(const QString& screenId)
{
    m_screenDesktops.remove(screenId);
    // The output is gone, and with it any report its old spelling could
    // still carry.
    m_renamedScreenIds.removeIf([&screenId](QHash<QString, QString>::iterator alias) {
        return alias.value() == screenId;
    });
}

bool VirtualDesktopManager::hasScreenDesktopReport(const QString& screenId) const
{
    if (m_screenDesktops.contains(screenId)) {
        return true;
    }
    if (PhosphorIdentity::VirtualScreenId::isVirtual(screenId)) {
        return m_screenDesktops.contains(PhosphorIdentity::VirtualScreenId::extractPhysicalId(screenId));
    }
    return false;
}

void VirtualDesktopManager::renameScreen(const QString& oldId, const QString& newId)
{
    // No row is ever stored under an empty id (updateScreenDesktop and
    // seedScreenDesktop refuse one), so an empty oldId finds nothing below.
    if (newId.isEmpty() || oldId == newId) {
        return;
    }
    // The effect may still report under the old spelling (see the header), so
    // it stays an alias of the new id. Whatever was aliased to the old id
    // follows it, and the new id, live again, is nobody's old spelling now.
    if (!oldId.isEmpty()) {
        m_renamedScreenIds.remove(newId);
        for (auto it = m_renamedScreenIds.begin(); it != m_renamedScreenIds.end(); ++it) {
            if (it.value() == oldId) {
                it.value() = newId;
            }
        }
        m_renamedScreenIds.insert(oldId, newId);
    }
    const auto from = m_screenDesktops.constFind(oldId);
    if (from == m_screenDesktops.constEnd()) {
        return; // nothing recorded under the dead id
    }
    const int desktop = from.value();
    m_screenDesktops.remove(oldId);
    // A live-id row already exists: it is a fresher report than the one we
    // would carry over, so it stands and the carried value is discarded. The
    // value under newId did not change, so nothing is emitted.
    if (m_screenDesktops.contains(newId)) {
        return;
    }
    m_screenDesktops.insert(newId, desktop);
    // A seed, not a switch: the output shows the desktop it showed, under a
    // new name, and the engines learn it under that name with no switch pass
    // and no OSD (F221).
    Q_EMIT screenDesktopSeeded(newId, desktop);
}

void VirtualDesktopManager::seedScreenDesktop(const QString& reportedScreenId, int desktop)
{
    if (reportedScreenId.isEmpty() || desktop < 1) {
        return;
    }
    const QString screenId = takeReportedScreenId(reportedScreenId);
    m_screenDesktops.insert(screenId, desktop);
    Q_EMIT screenDesktopSeeded(screenId, desktop);
}

void VirtualDesktopManager::setBaselineDesktopIds(const QStringList& desktopIds)
{
    if (desktopIds.isEmpty()) {
        return;
    }
    if (m_desktopIds.isEmpty()) {
        // No snapshot yet: the first one compares against this.
        m_baselineDesktopIds = desktopIds;
        return;
    }
    // The snapshot is already here, so the stores it renumbers are keyed on
    // the baseline's positions until this goes out. The per-screen mirror is
    // not touched: every screen in it was reported in the live numbering.
    const PhosphorEngine::DesktopRemap remap = PhosphorEngine::DesktopRemap::fromIds(desktopIds, m_desktopIds);
    if (!remap.isIdentity()) {
        Q_EMIT desktopsRenumbered(remap);
    }
}

void VirtualDesktopManager::clampScreenDesktopsToCount()
{
    // Clamp only entries above the live count: KWin renumbers on desktop
    // removal, so a screen pinned past the new count is pulled down to it here.
    // An entry whose desktop was RENUMBERED (a removal, an insert, a move) is
    // not this function's business: commitDesktopList moves those itself
    // through the position map, before this runs, and pulls down the ones a
    // removal at the END left out of range. What is left here is a count
    // committed with no list behind it: onNumberOfDesktopsChanged without a
    // list, and retryOrAbandonRefresh rolling a count back.
    //
    // Mutate first, then emit the captured value — emitting mid-iteration could
    // re-enter updateScreenDesktop and invalidate the hash iterator, and a
    // re-entrant write must not change the value we report for this clamp.
    QList<std::pair<QString, int>> clamped;
    for (auto it = m_screenDesktops.begin(); it != m_screenDesktops.end(); ++it) {
        if (it.value() > m_desktopCount) {
            it.value() = m_desktopCount;
            clamped.append({it.key(), m_desktopCount});
        }
    }
    for (const auto& [screenId, desktop] : clamped) {
        Q_EMIT screenDesktopChanged(screenId, desktop);
    }
}

void VirtualDesktopManager::noteDesktopListChanging()
{
    // Once per change (F1023). KWin sends several signals for one change (a
    // removal sends desktopDataChanged per shifted desktop, desktopRemoved and
    // countChanged), and the effect's pushes in the new numbering can arrive
    // between them. The window registry snapshots its records on this signal
    // and drops each one pushed since, so a second send re-armed records
    // already in the new numbering and the renumber moved them twice. The
    // commit ending the change clears the latch, so the NEXT change is
    // announced on its own even when it follows before a list read landed: a
    // record pushed between two changes is in the first one's numbering and
    // must be moved by the second (F466). Before the first snapshot there is
    // nothing live to renumber: that map compares against the previous session.
    if (!m_desktopListChangePending && !m_desktopIds.isEmpty()) {
        m_desktopListChangePending = true;
        Q_EMIT desktopListChanging();
    }
}

bool VirtualDesktopManager::liveListMatches(int count) const
{
    if (m_liveIds.size() != count || m_liveIds.isEmpty()) {
        return false;
    }
    const QSet<QString> unique(m_liveIds.cbegin(), m_liveIds.cend());
    return unique.size() == m_liveIds.size();
}

void VirtualDesktopManager::commitLiveList()
{
    QList<DesktopEntry> desktops;
    desktops.reserve(m_liveIds.size());
    for (int i = 0; i < m_liveIds.size(); ++i) {
        const QString& desktopId = m_liveIds.at(i);
        desktops.append(DesktopEntry{i, desktopId, m_liveRawNames.value(desktopId)});
    }
    // The current desktop's id: the one KWin named during the change, else
    // the one this manager had (a removed one resolves to its successor).
    const QString currentId = m_pendingCurrentId.isEmpty() ? desktopIdAt(m_currentDesktop) : m_pendingCurrentId;
    commitDesktopList(std::move(desktops), currentId, m_desktopRows);
}

void VirtualDesktopManager::setCurrentDesktop(int desktop)
{
    if (desktop < 1 || desktop > m_desktopCount) {
        return;
    }
    setCurrentDesktopById(desktopIdAt(desktop));
}

void VirtualDesktopManager::setCurrentDesktopById(const QString& desktopId)
{
    if (desktopId.isEmpty() || !m_available) {
        return;
    }
    // Only ids we actually know about. Writing an unknown UUID would
    // either be rejected by the compositor or, worse, switch to a desktop
    // this manager has no record of, leaving m_currentDesktop wrong until the
    // next refresh.
    if (!m_desktopIds.contains(desktopId)) {
        return;
    }
    // Not waited for: the switch arrives as currentChanged (F706).
    m_backend->setCurrentDesktop(desktopId);
}

void VirtualDesktopManager::onSourceCountChanged(int newCount)
{
    // Boundary validation: a compositor never reports 0, but KWin's is a
    // session-bus signal any peer can send, and committing 0 would publish an
    // invalid 1-based currentDesktopChanged(0) below until a refresh corrected
    // it.
    if (newCount < 1) {
        return;
    }

    // countChanged closes a create, a remove or a resize: every desktop
    // signal of that change came before it, so the live list is the
    // compositor's list now. Committing it here, rather than when a list read lands,
    // puts the renumber, the list and the count out together and ahead of
    // every report KWin sends after the change (Move to New Desktop's report
    // of desktop N+1 among them, F1022), and ends the change before the next
    // one can start (F466). The read already in flight still commits the
    // names and rows, and finds the list unchanged.
    if (m_desktopListChangePending && liveListMatches(newCount)) {
        commitLiveList();
        return;
    }
    if (m_desktopCount == newCount) {
        return;
    }

    // The signals did not add up to a list of this count (one was missed or
    // malformed), so the list read decides. A SHRINKING count is not committed
    // here: the read commits the count, the clamp and both signals together,
    // the renumber BEFORE the count. Committing it first published it ahead of
    // the map that says which desktop went, so a mid-list removal had the last
    // desktop's per-screen entry clamped and then moved again by the map, and
    // the daemon's count handler pruned the engine states past the new count
    // that the map was about to move down.
    //
    // A GROWING count is committed at once (F1022). Nothing is clamped or
    // pruned by it, and KWin sends countChanged before anything that uses the
    // new desktop, so every desktop range check (screenDesktopChanged,
    // setWindowMetadata) would otherwise refuse the effect's report of desktop
    // N+1 against the old count. The effect sends that report once. An insert
    // before the last desktop still renumbers when the read lands, after this
    // count went out, and a read that never lands rolls it back
    // (retryOrAbandonRefresh).
    if (m_available && m_backend->isReachable()) {
        noteDesktopListChanging();
        if (newCount > m_desktopCount) {
            m_desktopCount = newCount;
            Q_EMIT desktopCountChanged(m_desktopCount);
        }
        refreshFromSource();
        return;
    }

    m_desktopCount = newCount;

    // Clamp against the live member, not the signal arg, so this handler
    // stays self-consistent with desktopCount() at every point.
    if (m_currentDesktop > m_desktopCount) {
        m_currentDesktop = m_desktopCount;
        Q_EMIT currentDesktopChanged(m_currentDesktop);
    }

    clampScreenDesktopsToCount();

    // Report the live member, not the signal argument, for the same reason
    // the clamp above reads it: the member is what desktopCount() answers,
    // so emitting anything else would let a consumer's cached count
    // disagree with a follow-up read. Matches the emit in
    // commitDesktopList, which already sends m_desktopCount.
    Q_EMIT desktopCountChanged(m_desktopCount);
}

int VirtualDesktopManager::desktopCount() const
{
    return m_available ? m_desktopCount : 1;
}

int VirtualDesktopManager::desktopRows() const
{
    return m_available ? qMax(1, m_desktopRows) : 1;
}

QStringList VirtualDesktopManager::desktopNames() const
{
    if (m_available && !m_desktopNames.isEmpty()) {
        return m_desktopNames;
    }

    QStringList names;
    int count = desktopCount();
    for (int i = 1; i <= count; ++i) {
        names.append(fallbackDesktopName(i));
    }
    return names;
}

bool VirtualDesktopManager::isAvailable() const
{
    return m_available;
}

QStringList VirtualDesktopManager::desktopIds() const
{
    // No synthesised fallback, unlike desktopNames(): a made-up name is a
    // harmless placeholder, but a made-up id would be a key that matches
    // nothing in KWin, and setCurrentDesktopById would silently drop every
    // switch made against it. An empty list says "no stable ids here",
    // which a caller can act on.
    return m_desktopIds;
}

void VirtualDesktopManager::createDesktop(uint position, const QString& name)
{
    if (!m_available) {
        return;
    }
    // KWin's createDesktop returns nothing, and refuses past its desktop cap
    // by doing nothing, so the answer is the only way to tell that refusal
    // from a stall (F782). The backend answers after the echo the call caused.
    m_backend->createDesktop(position, name, [this, position, name](bool answered) {
        Q_EMIT createDesktopAnswered(position, name, answered ? CallAnswer::Answered : CallAnswer::Unanswered);
    });
}

void VirtualDesktopManager::removeDesktop(const QString& desktopId)
{
    if (!m_available || desktopId.isEmpty()) {
        return;
    }
    // Same contract as createDesktop's answer: KWin refuses the last desktop
    // and an unknown id by doing nothing.
    m_backend->removeDesktop(desktopId, [this, desktopId](bool answered) {
        Q_EMIT removeDesktopAnswered(desktopId, answered ? CallAnswer::Answered : CallAnswer::Unanswered);
    });
}

void VirtualDesktopManager::setDesktopName(const QString& desktopId, const QString& name)
{
    if (!m_available || desktopId.isEmpty()) {
        return;
    }
    m_backend->setDesktopName(desktopId, name);
}

QString VirtualDesktopManager::desktopIdAt(int desktop) const
{
    if (desktop < 1 || desktop > m_desktopIds.size()) {
        return QString();
    }
    return m_desktopIds.at(desktop - 1);
}

int VirtualDesktopManager::desktopIndexOf(const QString& desktopId) const
{
    return m_desktopIds.indexOf(desktopId) + 1;
}

QStringList VirtualDesktopManager::rawDesktopNames() const
{
    return m_available ? m_rawDesktopNames : QStringList();
}

} // namespace PhosphorWorkspaces
