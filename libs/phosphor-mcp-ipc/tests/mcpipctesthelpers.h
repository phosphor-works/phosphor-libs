// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// The application side of the tests: an IpcRouter on a socket in a
// temporary directory, with a C++ target that has overloads and a target
// that fails calls, and a QML IpcTarget whose untyped parameter takes any
// value. Header-only, like the helpers of phosphor-ipc and phosphor-mcp.

#include "ipctesthelpers.h"

#include <PhosphorIpc/IpcCallContext.h>
#include <PhosphorIpc/IpcEngine.h>
#include <PhosphorIpc/IpcPendingReply.h>
#include <PhosphorIpc/IpcRouter.h>
#include <PhosphorIpc/IpcTarget.h>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtCore/qtclasshelpermacros.h>

#include <list>
#include <memory>

namespace PhosphorMcpIpcTests {

class Panes : public QObject, protected PhosphorIpc::IpcCallContext
{
    Q_OBJECT
public:
    Panes() = default;
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
    // Answers only when the test says so.
    Q_INVOKABLE void wait(int pane)
    {
        Q_UNUSED(pane)
        waiting.push_back(deferReply());
    }
    // In no manifest of the tests.
    Q_INVOKABLE void secret()
    {
        ++secretCalls;
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

    std::list<PhosphorIpc::IpcPendingReply> waiting;
    int secretCalls = 0;

private:
    QHash<int, QString> m_titles;
};

// The router with its targets, "panes" in C++ and "script" in QML.
class Application
{
public:
    Application()
    {
        qmlRegisterType<PhosphorIpc::IpcTarget>("PhosphorMcpIpcTest", 1, 0, "IpcTarget");
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
        if (!m_fixture.valid() || !m_fixture.router.registerTarget(QStringLiteral("panes"), &panes)) {
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

    Panes panes;

private:
    PhosphorIpcTests::RouterFixture m_fixture;
    QQmlEngine m_engine;
    std::unique_ptr<QObject> m_script;
};

// The manifest of most tests: some calls of each target, a target the
// socket does not have, and a function its target does not have. Neither
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
    {"call": "panes.nothing", "name": "nothing", "description": "A function the target does not have."}
]})");
}

} // namespace PhosphorMcpIpcTests
