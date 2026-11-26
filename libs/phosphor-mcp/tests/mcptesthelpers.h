// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// The client side of the tests: one end of a socket pair, which the test
// writes requests to and reads replies from while the event loop runs, so
// that the server on the other end, in the same thread, gets to answer.
// Header-only, like phosphor-ipc's test helpers: each binary has its own
// QTest main.

#include <PhosphorMcp/McpProtocol.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDebug>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QtCore/qtclasshelpermacros.h>

#include <cerrno>
#include <climits>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace PhosphorMcpTests {

// Runs the event loop for up to `ms`, or until `done` holds.
template<typename Done>
bool spinUntil(Done done, int ms = 5000)
{
    QDeadlineTimer deadline(ms);
    while (!done()) {
        if (deadline.hasExpired()) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents();
        ::poll(nullptr, 0, 1);
    }
    return true;
}

inline void spinFor(int ms)
{
    spinUntil(
        [] {
            return false;
        },
        ms);
}

class Peer
{
public:
    // The send buffer of the server's end, fixed so that what the socket
    // takes before a write would block does not depend on the machine's
    // net.core.wmem_default. The kernel doubles it, and a reply of a few
    // hundred kilobytes already waits in the server's queue.
    static constexpr int ServerSendBufferBytes = 64 * 1024;

    Peer()
    {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
            return;
        }
        const int sendBuffer = ServerSendBufferBytes;
        if (::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof sendBuffer) != 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return;
        }
        m_server = fds[0];
        m_client = fds[1];
        ::fcntl(m_client, F_SETFL, ::fcntl(m_client, F_GETFL) | O_NONBLOCK);
    }
    ~Peer()
    {
        closeClient();
        if (m_server >= 0) {
            ::close(m_server);
        }
    }
    Q_DISABLE_COPY_MOVE(Peer)

    [[nodiscard]] bool isValid() const
    {
        return m_server >= 0 && m_client >= 0;
    }
    // The descriptor the server reads and writes.
    [[nodiscard]] int serverFd() const
    {
        return m_server;
    }
    [[nodiscard]] int clientFd() const
    {
        return m_client;
    }

    // Writes `bytes` whole, running the event loop while the socket is full
    // so that the server can read.
    bool sendRaw(const QByteArray& bytes)
    {
        qsizetype offset = 0;
        QDeadlineTimer deadline(10000);
        while (offset < bytes.size()) {
            const ssize_t written = ::write(m_client, bytes.constData() + offset, size_t(bytes.size() - offset));
            if (written > 0) {
                offset += written;
                continue;
            }
            if (written < 0 && errno != EAGAIN && errno != EINTR) {
                return false;
            }
            if (deadline.hasExpired()) {
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        return true;
    }
    bool send(const QJsonObject& message)
    {
        return sendRaw(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    }

    // Ends the server's input, as a client that closes stdin does.
    void closeWrite()
    {
        ::shutdown(m_client, SHUT_WR);
    }
    // Closes the client's end, as a client that went away does. The server
    // then reads the end of its input, or ECONNRESET when replies were left
    // unread, and its writes fail with EPIPE.
    void closeClient()
    {
        if (m_client >= 0) {
            ::close(m_client);
            m_client = -1;
        }
    }

    // Runs the event loop until the server has read everything sent so far,
    // which the socket no longer holds. False after `ms` without that.
    bool waitUntilServerRead(int ms = 5000)
    {
        return spinUntil(
            [this] {
                int unread = 0;
                return ::ioctl(m_server, FIONREAD, &unread) == 0 && unread == 0;
            },
            ms);
    }

    // Takes up to `maxBytes` of what has come, without waiting. Returns the
    // bytes read. It is 0 both when nothing has come yet and at the end of
    // the stream, which ended() then tells.
    qint64 pump(qint64 maxBytes = 1 << 20)
    {
        QByteArray chunk(maxBytes, Qt::Uninitialized);
        const ssize_t got = ::read(m_client, chunk.data(), size_t(chunk.size()));
        if (got > 0) {
            m_input.append(chunk.constData(), got);
            m_bytesRead += got;
            return got;
        }
        if (got == 0) {
            m_ended = true;
        }
        return 0;
    }

    // The messages that came, parsed, as they arrive while the event loop
    // runs: `count` of them, or what came before `ms` passed. A line that
    // is not a JSON object comes out as {"unparsed": line}.
    QList<QJsonObject> read(int count, int ms = 5000)
    {
        QList<QJsonObject> messages;
        spinUntil(
            [&] {
                pump();
                takeMessages(messages, count);
                return messages.size() >= count || m_ended;
            },
            ms);
        takeMessages(messages, count);
        return messages;
    }
    // Everything that comes within `ms`.
    QList<QJsonObject> readFor(int ms = 200)
    {
        QList<QJsonObject> messages;
        spinUntil(
            [&] {
                pump();
                takeMessages(messages, INT_MAX);
                return m_ended;
            },
            ms);
        takeMessages(messages, INT_MAX);
        return messages;
    }
    QJsonObject readOne(int ms = 5000)
    {
        const QList<QJsonObject> messages = read(1, ms);
        return messages.isEmpty() ? QJsonObject() : messages.first();
    }

    [[nodiscard]] qint64 bytesRead() const
    {
        return m_bytesRead;
    }
    // The server's end of the stream came: its descriptor was closed or
    // shut down for writing.
    [[nodiscard]] bool ended() const
    {
        return m_ended;
    }

private:
    void takeMessages(QList<QJsonObject>& messages, int count)
    {
        while (messages.size() < count) {
            const qsizetype newline = m_input.indexOf('\n');
            if (newline < 0) {
                return;
            }
            const QByteArray line = m_input.left(newline);
            m_input.remove(0, newline + 1);
            const QJsonDocument document = QJsonDocument::fromJson(line);
            if (document.isObject()) {
                messages.append(document.object());
            } else {
                messages.append(QJsonObject{{QLatin1StringView("unparsed"), QString::fromUtf8(line)}});
            }
        }
    }

    int m_server = -1;
    int m_client = -1;
    QByteArray m_input;
    qint64 m_bytesRead = 0;
    bool m_ended = false;
};

inline QJsonObject meta(const QString& version = QString(PhosphorMcp::ProtocolVersion),
                        const QJsonObject& capabilities = QJsonObject())
{
    return QJsonObject{
        {PhosphorMcp::MetaProtocolVersion, version},
        {PhosphorMcp::MetaClientCapabilities, capabilities},
        {PhosphorMcp::MetaClientInfo,
         QJsonObject{{QLatin1StringView("name"), QStringLiteral("test")},
                     {QLatin1StringView("version"), QStringLiteral("1")}}},
    };
}

inline QJsonObject request(const QJsonValue& id, const QString& method, QJsonObject params = QJsonObject(),
                           const QJsonObject& requestMeta = meta())
{
    if (!params.contains(QLatin1StringView("_meta"))) {
        params.insert(QLatin1StringView("_meta"), requestMeta);
    }
    return QJsonObject{
        {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
        {QLatin1StringView("id"), id},
        {QLatin1StringView("method"), method},
        {QLatin1StringView("params"), params},
    };
}

inline QJsonObject callRequest(const QJsonValue& id, const QString& tool, const QJsonObject& arguments = QJsonObject(),
                               const QJsonObject& requestMeta = meta())
{
    return request(id, QStringLiteral("tools/call"),
                   QJsonObject{
                       {QLatin1StringView("name"), tool},
                       {QLatin1StringView("arguments"), arguments},
                       {QLatin1StringView("_meta"), requestMeta},
                   });
}

inline QJsonObject notification(const QString& method, const QJsonObject& params = QJsonObject())
{
    return QJsonObject{
        {QLatin1StringView("jsonrpc"), QStringLiteral("2.0")},
        {QLatin1StringView("method"), method},
        {QLatin1StringView("params"), params},
    };
}

inline QJsonObject cancelNotification(const QJsonValue& requestId)
{
    return notification(QStringLiteral("notifications/cancelled"),
                        QJsonObject{{QLatin1StringView("requestId"), requestId},
                                    {QLatin1StringView("reason"), QStringLiteral("test")}});
}

inline int errorCode(const QJsonObject& message)
{
    return message.value(QLatin1StringView("error")).toObject().value(QLatin1StringView("code")).toInt();
}

inline QJsonObject resultOf(const QJsonObject& message)
{
    return message.value(QLatin1StringView("result")).toObject();
}

// The text of the first content block of a tools/call result.
inline QString firstText(const QJsonObject& message)
{
    return resultOf(message)
        .value(QLatin1StringView("content"))
        .toArray()
        .first()
        .toObject()
        .value(QLatin1StringView("text"))
        .toString();
}

// The JSON object of a literal in a test. A literal that is no JSON object
// is a mistake of the test, and ends it: an empty object in its place
// would compare equal to the result of a reply that never came.
inline QJsonObject parseObject(const char* json)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qFatal("a JSON literal of the test is not an object (%s): %s", qPrintable(error.errorString()), json);
    }
    return document.object();
}

// The name and the version of the server, which each result holds.
inline QJsonObject serverInfo(const QJsonObject& reply)
{
    return resultOf(reply).value(QLatin1StringView("_meta")).toObject().value(PhosphorMcp::MetaServerInfo).toObject();
}

// A server program, run as an MCP client runs it. It is a child process
// with pipes for stdin and stdout, in its own session without a terminal.
// Its stderr goes to the test's output when a check fails, and the process
// is killed if it still runs at the end. The waits run the event loop of
// the test, so that objects of the test can answer the child meanwhile.
class ChildServer
{
public:
    // `label` names the program in the test's output.
    ChildServer(const QString& program, const QString& label)
        : m_program(program)
        , m_label(label)
    {
        // Without QT_FORCE_STDERR_LOGGING, as a client starts it: the
        // diagnostics of the program then reach stderr only because it asks
        // Qt for that itself. Without the logging rules of the person who
        // runs the test, which can turn those diagnostics off.
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("QT_FORCE_STDERR_LOGGING"));
        environment.remove(QStringLiteral("QT_LOGGING_RULES"));
        environment.remove(QStringLiteral("QT_LOGGING_CONF"));
        m_process.setProcessEnvironment(environment);
        m_process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
    }
    ~ChildServer()
    {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            m_process.waitForFinished(5000);
        }
        if (QTest::currentTestFailed()) {
            qWarning().noquote() << "stderr of" << m_label << ":\n" << QString::fromUtf8(standardError());
        }
    }
    Q_DISABLE_COPY_MOVE(ChildServer)

    [[nodiscard]] bool start(const QStringList& arguments = {})
    {
        m_process.start(m_program, arguments);
        return m_process.waitForStarted(5000);
    }

    // True when the whole line went into the pipe's buffer.
    [[nodiscard]] bool write(const QJsonObject& message)
    {
        const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        return m_process.write(line) == line.size();
    }

    // Up to `count` replies, fewer when the process ends or 10 s pass first.
    [[nodiscard]] QList<QJsonObject> readReplies(int count)
    {
        QList<QJsonObject> replies;
        spinUntil(
            [&] {
                while (m_process.canReadLine() && replies.size() < count) {
                    replies.append(QJsonDocument::fromJson(m_process.readLine()).object());
                }
                return replies.size() == count || m_process.state() == QProcess::NotRunning;
            },
            10000);
        return replies;
    }

    [[nodiscard]] bool waitForExit()
    {
        return spinUntil(
            [&] {
                return m_process.state() == QProcess::NotRunning;
            },
            10000);
    }

    // Everything the process wrote to stderr so far.
    [[nodiscard]] QByteArray standardError()
    {
        m_standardError += m_process.readAllStandardError();
        return m_standardError;
    }

    [[nodiscard]] QProcess& process()
    {
        return m_process;
    }

private:
    QString m_program;
    QString m_label;
    QProcess m_process;
    QByteArray m_standardError;
};

} // namespace PhosphorMcpTests
