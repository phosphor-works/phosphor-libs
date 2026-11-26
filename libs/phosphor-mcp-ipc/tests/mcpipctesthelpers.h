// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// The application side of the tests: an IpcRouter on a socket in a
// temporary directory, with two targets. "panes" is a C++ object with
// overloads and with methods that fail or answer later. "script" is a QML
// IpcTarget whose untyped parameter takes any value. Header-only, like the
// helpers of phosphor-ipc and phosphor-mcp.

#include "ipctesthelpers.h"

#include <PhosphorIpc/IpcCallContext.h>
#include <PhosphorIpc/IpcEngine.h>
#include <PhosphorIpc/IpcPendingReply.h>
#include <PhosphorIpc/IpcRouter.h>
#include <PhosphorIpc/IpcTarget.h>

#include <QByteArray>
#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtCore/qtclasshelpermacros.h>

#include <list>
#include <memory>

namespace PhosphorMcpIpcTests {

class Panes : public QObject, protected PhosphorIpc::IpcCallContext
{
    Q_OBJECT
public:
    explicit Panes(PhosphorIpc::IpcRouter& router)
        : m_router(router)
    {
    }
    Q_DISABLE_COPY_MOVE(Panes)

    Q_INVOKABLE QJsonObject describe(int pane) const
    {
        return QJsonObject{{QStringLiteral("pane"), pane}, {QStringLiteral("title"), m_titles.value(pane)}};
    }
    Q_INVOKABLE QString rename(int pane, const QString& title)
    {
        m_titles.insert(pane, title);
        return QStringLiteral("pane %1 is now %2").arg(pane).arg(title);
    }
    Q_INVOKABLE QString split(int pane)
    {
        return QStringLiteral("split %1").arg(pane);
    }
    Q_INVOKABLE QString split(int pane, const QString& direction)
    {
        return QStringLiteral("split %1 %2").arg(pane).arg(direction);
    }
    Q_INVOKABLE int close(int pane)
    {
        if (pane != 1) {
            sendErrorReply(QStringLiteral("no pane %1").arg(pane));
        }
        return pane;
    }
    Q_INVOKABLE QStringList names() const
    {
        return {QStringLiteral("a"), QStringLiteral("b")};
    }
    // Answers when the test finishes the reply in waiting().
    Q_INVOKABLE void wait(int pane)
    {
        Q_UNUSED(pane)
        m_waiting.push_back(deferReply());
    }
    // In no manifest of the tests.
    Q_INVOKABLE void secret()
    {
        ++m_secretCalls;
    }
    // Two overloads with one parameter each: the router cannot tell them
    // apart, so no manifest may choose one.
    Q_INVOKABLE QString focus(int pane)
    {
        return QString::number(pane);
    }
    Q_INVOKABLE QString focus(const QString& title)
    {
        return title;
    }
    // A default argument. The method is then callable with one argument
    // and with two, as two overloads.
    Q_INVOKABLE QString zoom(int pane, int factor = 2)
    {
        return QStringLiteral("zoom %1 by %2").arg(pane).arg(factor);
    }
    // An overload without parameters beside one with a parameter.
    Q_INVOKABLE QString layout()
    {
        return QStringLiteral("the layout");
    }
    Q_INVOKABLE QString layout(const QString& name)
    {
        return QStringLiteral("the layout %1").arg(name);
    }
    // One parameter of each kind that the schema can describe. The last is
    // a type without a JSON form.
    Q_INVOKABLE bool kinds(bool flag, double ratio, const QStringList& tags, const QVariantList& items,
                           const QVariantMap& options, const QUrl& link)
    {
        return flag && ratio > 0 && tags.size() == 2 && items.size() == 2
            && options.value(QStringLiteral("k")).toInt() == 1 && link.host() == QLatin1String("example.org");
    }
    Q_INVOKABLE void nothing()
    {
    }
    // Sends an event to its caller ahead of the reply.
    Q_INVOKABLE QString install(const QString& name)
    {
        m_router.sendEvent(connectionId(), QStringLiteral("panes"), QStringLiteral("output"),
                           QJsonArray{QStringLiteral("fetching %1").arg(name)});
        return QStringLiteral("installed %1").arg(name);
    }
    // More parameters than a call takes.
    Q_INVOKABLE int eleven(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, int k)
    {
        return a + b + c + d + e + f + g + h + i + j + k;
    }
    // A parameter without a name, which no argument can name.
    Q_INVOKABLE int unnamed(int)
    {
        return 0;
    }

    std::list<PhosphorIpc::IpcPendingReply>& waiting()
    {
        return m_waiting;
    }
    int secretCalls() const
    {
        return m_secretCalls;
    }

private:
    PhosphorIpc::IpcRouter& m_router;
    std::list<PhosphorIpc::IpcPendingReply> m_waiting;
    int m_secretCalls = 0;
    QHash<int, QString> m_titles;
};

// The router with its targets, "panes" in C++ and "script" in QML.
class Application
{
public:
    Application()
    {
        // The registration is for the process. One is enough for every test.
        static const int registered = qmlRegisterType<PhosphorIpc::IpcTarget>("PhosphorMcpIpcTest", 1, 0, "IpcTarget");
        Q_UNUSED(registered)
        PhosphorIpc::IpcEngine::install(&m_engine, &m_fixture.router);
    }
    ~Application()
    {
        m_script.reset();
        m_fixture.router.stop();
    }
    Q_DISABLE_COPY_MOVE(Application)

    // Starts the router with both targets. False on any failure.
    bool start()
    {
        if (!m_fixture.valid() || !m_fixture.router.registerTarget(QStringLiteral("panes"), &m_panes)) {
            return false;
        }
        QQmlComponent component(&m_engine);
        // run() is untyped, as most QML functions are: its parameter and
        // its return value are QVariant, and its schema has no type.
        component.setData(R"(import PhosphorMcpIpcTest
IpcTarget {
    target: "script"
    function run(value) { return { got: value } }
    function greet(name: string): string { return "hi " + name }
})",
                          QUrl());
        m_script.reset(component.create());
        if (!m_script) {
            qWarning().noquote() << component.errorString();
            return false;
        }
        return m_fixture.router.start(m_fixture.sockPath);
    }

    PhosphorIpc::IpcRouter& router()
    {
        return m_fixture.router;
    }
    QString socketPath() const
    {
        return m_fixture.sockPath;
    }
    Panes& panes()
    {
        return m_panes;
    }

private:
    // The router first: the panes target sends events through it.
    PhosphorIpcTests::RouterFixture m_fixture;
    Panes m_panes{m_fixture.router};
    QQmlEngine m_engine;
    std::unique_ptr<QObject> m_script;
};

// The manifest of most tests: some calls of each target, a target the
// socket does not have, and a method its target does not have. Neither
// panes.secret nor panes.focus is in it.
inline QByteArray manifestJson()
{
    return QByteArrayLiteral(R"({"tools": [
    {"call": "panes.describe", "name": "describe_pane", "title": "Describe a pane",
     "description": "Describes a pane.",
     "annotations": {"readOnly": true, "destructive": false, "idempotent": true, "openWorld": false}},
    {"call": "panes.rename", "name": "rename_pane", "description": "Renames a pane."},
    {"call": "panes.split", "name": "split_pane", "description": "Splits a pane.", "params": ["pane", "direction"]},
    {"call": "panes.close", "name": "close_pane", "description": "Closes a pane."},
    {"call": "panes.names", "name": "pane_names", "description": "Names the panes."},
    {"call": "panes.wait", "name": "wait_pane", "description": "Waits for a pane."},
    {"call": "script.run", "name": "run_script", "description": "Runs a script."},
    {"call": "script.greet", "name": "greet", "description": "Greets someone."},
    {"call": "ghost.haunt", "name": "haunt", "description": "A target the socket does not have."},
    {"call": "panes.missing", "name": "missing", "description": "A method the target does not have."}
]})");
}

} // namespace PhosphorMcpIpcTests
