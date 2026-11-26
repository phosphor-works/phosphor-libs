// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// Shared NDJSON drain / round-trip helpers used by several
// phosphor-ipc test binaries. Header-only because each test
// binary links its own QTest main and we don't want a shared
// .cpp dragging extra symbols into every binary's MOC.

#include <PhosphorIpc/IpcProtocol.h>
#include <PhosphorIpc/IpcRouter.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <optional>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace PhosphorIpcTests {

// Connect `socket` to the router at `path`. True once connected.
inline bool connectClient(QLocalSocket& socket, const QString& path)
{
    socket.connectToServer(path);
    return socket.waitForConnected(2000);
}

// Write one request line and flush it.
inline void send(QLocalSocket& socket, const QJsonObject& request)
{
    socket.write(PhosphorIpc::writeLine(request));
    socket.flush();
}

// Drain a QLocalSocket until N response lines arrive or timeout
// fires. Returns the parsed objects in arrival order. Pre-drains
// bytes already buffered so a second call against the same socket
// doesn't deadlock waiting for a readyRead that already fired. Takes
// whole lines only: a line still arriving stays in the socket for the
// next call.
inline QList<QJsonObject> readLines(QLocalSocket& socket, int expectedCount, int timeoutMs = 2000)
{
    QList<QJsonObject> out;

    auto drain = [&]() {
        while (socket.canReadLine()) {
            QByteArray line = socket.readLine();
            while (!line.isEmpty() && (line.endsWith('\n') || line.endsWith('\r'))) {
                line.chop(1);
            }
            if (line.isEmpty()) {
                continue;
            }
            out.append(QJsonDocument::fromJson(line).object());
        }
    };

    drain();
    if (out.size() >= expectedCount) {
        return out;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QLocalSocket::readyRead, &loop, [&] {
        drain();
        if (out.size() >= expectedCount) {
            loop.quit();
        }
    });
    timeout.start(timeoutMs);
    loop.exec();
    return out;
}

// Build a wire-format NDJSON request object. Optional fields are
// omitted when empty; the protocol parser treats absent fields the
// same as missing ones.
inline QJsonObject makeReq(const QString& type, qint64 id, const QString& target = {}, const QString& signal = {},
                           qint64 subId = 0)
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), type);
    o.insert(QStringLiteral("id"), static_cast<double>(id));
    if (!target.isEmpty()) {
        o.insert(QStringLiteral("target"), target);
    }
    if (!signal.isEmpty()) {
        o.insert(QStringLiteral("signal"), signal);
    }
    if (subId != 0) {
        o.insert(QStringLiteral("subscriptionId"), static_cast<double>(subId));
    }
    return o;
}

// Build a wire-format call request (type/id/target/fn/args). The args
// array is appended verbatim; pass {} for a no-arg call.
inline QJsonObject makeCallReq(qint64 id, const QString& target, const QString& fn, const QJsonArray& args = {})
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("call"));
    o.insert(QStringLiteral("id"), static_cast<double>(id));
    o.insert(QStringLiteral("target"), target);
    o.insert(QStringLiteral("fn"), fn);
    if (!args.isEmpty()) {
        o.insert(QStringLiteral("args"), args);
    }
    return o;
}

// Per-test router fixture: an isolated socket dir + path + router. Collapses
// the QTemporaryDir/sockPath/IpcRouter boilerplate every e2e/subscribe test
// otherwise repeats. Tests register their targets and call
// `router.start(fx.sockPath)` themselves (target sets vary per test).
struct RouterFixture
{
    QTemporaryDir dir;
    QString sockPath;
    PhosphorIpc::IpcRouter router;

    RouterFixture()
        : sockPath(QDir(dir.path()).filePath(QStringLiteral("test.sock")))
    {
    }
    bool valid() const
    {
        return dir.isValid();
    }
};

// One-shot connect + send + drain a single response line. Failures
// (connect timeout, response timeout) populate outError and return
// std::nullopt so callers can QFAIL with a precise diagnostic
// instead of a downstream "compared empty object to expected reply".
inline std::optional<QJsonObject> roundtrip(const QString& socketPath, const QJsonObject& request, QString* outError,
                                            int timeoutMs = 2000)
{
    QLocalSocket socket;
    QByteArray buffer;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);

    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QLocalSocket::readyRead, &loop, [&] {
        buffer.append(socket.readAll());
        if (buffer.contains('\n')) {
            loop.quit();
        }
    });

    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(timeoutMs)) {
        if (outError) {
            *outError = QStringLiteral("connect failed: %1").arg(socket.errorString());
        }
        return std::nullopt;
    }
    socket.write(PhosphorIpc::writeLine(request));
    socket.flush();

    timeout.start(timeoutMs);
    loop.exec();

    if (!buffer.contains('\n')) {
        if (outError) {
            *outError = QStringLiteral("no response within %1ms").arg(timeoutMs);
        }
        return std::nullopt;
    }
    const int nl = buffer.indexOf('\n');
    QByteArray line = buffer.left(nl);
    while (!line.isEmpty() && line.endsWith('\r')) {
        line.chop(1);
    }
    socket.disconnectFromServer();
    return QJsonDocument::fromJson(line).object();
}

// A listening Unix socket that the test drives by hand, for what a real
// IpcRouter never does. It holds a connection back, never reads, sends a
// reply in two parts, sends a line that is no frame, or closes in the
// middle. Every wait spins the event loop of the test, because the client
// under test reads and writes from it.
class RawServer
{
public:
    RawServer()
        : m_path(QDir(m_dir.path()).filePath(QStringLiteral("raw.sock")))
    {
    }
    ~RawServer()
    {
        closeConnection();
        closeHeld();
        closeListener();
    }
    RawServer(const RawServer&) = delete;
    RawServer& operator=(const RawServer&) = delete;

    [[nodiscard]] QString path() const
    {
        return m_path;
    }

    // Listens at path(). The kernel queues `backlog` + 1 connections that
    // are not accepted yet.
    [[nodiscard]] bool listen(int backlog = 16)
    {
        const QByteArray name = m_path.toLocal8Bit();
        sockaddr_un address{};
        if (!m_dir.isValid() || name.size() >= qsizetype(sizeof(address.sun_path))) {
            return false;
        }
        m_listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_listener < 0) {
            return false;
        }
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, name.constData(), std::size_t(name.size()));
        return ::bind(m_listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0
            && ::listen(m_listener, backlog) == 0;
    }

    // Fills the queue of a listen(0) socket with a connection of the
    // test's own, so that the next client stays in its connecting state
    // until acceptHeld() or closeListener(). Linux only: another kernel
    // may refuse the client or queue it.
    [[nodiscard]] bool holdNextConnection()
    {
        const QByteArray name = m_path.toLocal8Bit();
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, name.constData(), std::size_t(name.size()));
        m_filler = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        return m_filler >= 0 && ::connect(m_filler, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }

    // Takes the connection of holdNextConnection() out of the queue, and
    // then accepts the client that waited behind it.
    [[nodiscard]] bool acceptHeld(int timeoutMs = 2000)
    {
        const int filler = ::accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (filler < 0) {
            return false;
        }
        ::close(filler);
        closeHeld();
        return accept(timeoutMs);
    }

    // Accepts the next connection. It becomes the one that the other
    // functions read and write. An earlier one stays open.
    [[nodiscard]] bool accept(int timeoutMs = 2000)
    {
        if (!waitReadable(m_listener, timeoutMs)) {
            return false;
        }
        // Without waiting: write() gives the event loop a turn when the
        // socket is full, so that the client under test can read.
        const int connection = ::accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (connection < 0) {
            return false;
        }
        if (m_connection >= 0) {
            m_earlier.append(m_connection);
        }
        m_connection = connection;
        m_buffer.clear();
        return true;
    }

    // The next request line of the client, as an object. Nothing when
    // none comes in time, the client closed, or the line is no object.
    [[nodiscard]] std::optional<QJsonObject> readRequest(int timeoutMs = 2000)
    {
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            const qsizetype newline = m_buffer.indexOf('\n');
            if (newline >= 0) {
                const QJsonDocument document = QJsonDocument::fromJson(m_buffer.left(newline));
                m_buffer.remove(0, newline + 1);
                return document.isObject() ? std::optional<QJsonObject>(document.object()) : std::nullopt;
            }
            const qint64 left = timeoutMs - timer.elapsed();
            if (left <= 0 || !waitReadable(m_connection, int(left))) {
                return std::nullopt;
            }
            char chunk[65536];
            const ssize_t count = ::recv(m_connection, chunk, sizeof(chunk), 0);
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;
            }
            if (count <= 0) {
                return std::nullopt;
            }
            m_buffer.append(chunk, qsizetype(count));
        }
    }

    // Writes bytes to the client as they are.
    [[nodiscard]] bool write(const QByteArray& bytes)
    {
        QElapsedTimer timer;
        timer.start();
        qsizetype written = 0;
        while (written < bytes.size()) {
            const ssize_t count =
                ::send(m_connection, bytes.constData() + written, std::size_t(bytes.size() - written), MSG_NOSIGNAL);
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
                // The socket is full. The client reads from the event loop.
                if (timer.elapsed() > 10000) {
                    return false;
                }
                QCoreApplication::processEvents();
                pollfd entry{m_connection, POLLOUT, 0};
                ::poll(&entry, 1, 10);
                continue;
            }
            if (count <= 0) {
                return false;
            }
            written += count;
        }
        return true;
    }
    [[nodiscard]] bool write(const QJsonObject& frame)
    {
        return write(PhosphorIpc::writeLine(frame));
    }
    [[nodiscard]] bool reply(qint64 id, const QJsonValue& result)
    {
        return write(PhosphorIpc::buildReply(id, result));
    }

    // True when the client closed its end, within the time.
    [[nodiscard]] bool clientClosed(int timeoutMs = 2000)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            if (!waitReadable(m_connection, int(timeoutMs - timer.elapsed()))) {
                return false;
            }
            char chunk[65536];
            const ssize_t count = ::recv(m_connection, chunk, sizeof(chunk), 0);
            if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR)) {
                return true;
            }
        }
        return false;
    }

    void closeConnection()
    {
        for (const int earlier : std::as_const(m_earlier)) {
            ::close(earlier);
        }
        m_earlier.clear();
        if (m_connection >= 0) {
            ::close(m_connection);
            m_connection = -1;
        }
    }

    // Stops listening. A client that waits in the queue is refused.
    void closeListener()
    {
        if (m_listener >= 0) {
            ::close(m_listener);
            m_listener = -1;
            ::unlink(m_path.toLocal8Bit().constData());
        }
    }

private:
    void closeHeld()
    {
        if (m_filler >= 0) {
            ::close(m_filler);
            m_filler = -1;
        }
    }

    // Spins the event loop until `fd` has something to read.
    static bool waitReadable(int fd, int timeoutMs)
    {
        if (fd < 0) {
            return false;
        }
        QElapsedTimer timer;
        timer.start();
        do {
            QCoreApplication::processEvents();
            pollfd entry{fd, POLLIN, 0};
            if (::poll(&entry, 1, 2) > 0) {
                return true;
            }
        } while (timer.elapsed() < timeoutMs);
        return false;
    }

    QTemporaryDir m_dir;
    QString m_path;
    int m_listener = -1;
    int m_filler = -1;
    int m_connection = -1;
    QList<int> m_earlier;
    QByteArray m_buffer;
};

} // namespace PhosphorIpcTests
