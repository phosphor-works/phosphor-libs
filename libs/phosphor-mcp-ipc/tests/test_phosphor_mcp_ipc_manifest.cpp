// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The manifest's shape, which is strict: what it does not name is an error.

#include <PhosphorMcpIpc/McpIpcManifest.h>

#include <QDir>
#include <QFile>
#include <QObject>
#include <QTemporaryDir>
#include <QTest>

using namespace PhosphorMcpIpc;

class TestPhosphorMcpIpcManifest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readsEveryMember()
    {
        QString error;
        const std::optional<McpIpcManifest> manifest = McpIpcManifest::parse(R"({"tools": [
            {"call": "app.window.close", "name": "close_window", "title": "Close", "description": "Closes a window.",
             "params": ["window", "force"],
             "annotations": {"readOnly": false, "destructive": true, "idempotent": false, "openWorld": false}},
            {"call": "app.list", "name": "list", "description": "Lists.", "params": [], "annotations": {"readOnly": true}}
        ]})",
                                                                             &error);
        QVERIFY2(manifest, qPrintable(error));
        QCOMPARE(manifest->tools.size(), 2);

        const McpIpcManifestTool& close = manifest->tools.at(0);
        // The target may hold dots, a function cannot.
        QCOMPARE(close.target, QStringLiteral("app.window"));
        QCOMPARE(close.function, QStringLiteral("close"));
        QCOMPARE(close.name, QStringLiteral("close_window"));
        QCOMPARE(close.title, QStringLiteral("Close"));
        QCOMPARE(close.description, QStringLiteral("Closes a window."));
        QCOMPARE(close.params,
                 std::optional<QStringList>(QStringList{QStringLiteral("window"), QStringLiteral("force")}));
        QCOMPARE(close.readOnly, std::optional<bool>(false));
        QCOMPARE(close.destructive, std::optional<bool>(true));
        QCOMPARE(close.idempotent, std::optional<bool>(false));
        QCOMPARE(close.openWorld, std::optional<bool>(false));

        const McpIpcManifestTool& list = manifest->tools.at(1);
        QVERIFY(list.title.isEmpty());
        // An empty list chooses the overload without parameters, which no
        // list at all does not.
        QCOMPARE(list.params, std::optional<QStringList>(QStringList()));
        QCOMPARE(list.readOnly, std::optional<bool>(true));
        QVERIFY(!list.destructive && !list.idempotent && !list.openWorld);
    }

    void refusesWhatItDoesNotName_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("error");
        QTest::newRow("not JSON") << QByteArray("{") << QStringLiteral("not JSON at offset 1: unterminated object");
        QTest::newRow("not an object") << QByteArray("[]") << QStringLiteral("the root is not an object");
        QTest::newRow("no tools") << QByteArray("{}") << QStringLiteral("\"tools\" is not an array");
        QTest::newRow("unknown root member")
            << QByteArray(R"({"tools": [], "server": "x"})") << QStringLiteral("unknown member \"server\" at the root");
        QTest::newRow("tool not an object")
            << QByteArray(R"({"tools": [1]})") << QStringLiteral("tools[0] is not an object");
        QTest::newRow("unknown tool member")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "parms": ["x"]}]})")
            << QStringLiteral("tools[0]: unknown member \"parms\"");
        QTest::newRow("call without a dot")
            << QByteArray(R"({"tools": [{"call": "ab", "name": "n", "description": "d"}]})")
            << QStringLiteral("tools[0]: \"call\" is not a string of the form TARGET.METHOD");
        QTest::newRow("call ending in a dot")
            << QByteArray(R"({"tools": [{"call": "a.", "name": "n", "description": "d"}]})")
            << QStringLiteral("tools[0]: \"call\" is not a string of the form TARGET.METHOD");
        QTest::newRow("no name") << QByteArray(R"({"tools": [{"call": "a.b", "description": "d"}]})")
                                 << QStringLiteral("tools[0]: \"name\" is not a string that is not empty");
        QTest::newRow("no description") << QByteArray(R"({"tools": [{"call": "a.b", "name": "n"}]})")
                                        << QStringLiteral(
                                               "tools[0]: \"description\" is not a string that is not empty");
        QTest::newRow("title not a string")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "title": 1}]})")
            << QStringLiteral("tools[0]: \"title\" is not a string");
        QTest::newRow("params not strings")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "params": [1]}]})")
            << QStringLiteral("tools[0]: \"params\" holds a value that is not a string that is not empty");
        QTest::newRow("params twice") << QByteArray(
            R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "params": ["x", "x"]}]})")
                                      << QStringLiteral("tools[0]: \"params\" names \"x\" twice");
        QTest::newRow("unknown annotation") << QByteArray(
            R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "annotations": {"readonly": true}}]})")
                                            << QStringLiteral("tools[0]: unknown member \"annotations.readonly\"");
        QTest::newRow("annotation not a boolean") << QByteArray(
            R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "annotations": {"readOnly": 1}}]})")
                                                  << QStringLiteral("tools[0]: annotations.readOnly is not a boolean");
        QTest::newRow("same name twice") << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": "d"},
                                                      {"call": "a.c", "name": "n", "description": "d"}]})")
                                         << QStringLiteral("tools[1]: another tool is named \"n\"");
    }

    void refusesWhatItDoesNotName()
    {
        QFETCH(QByteArray, json);
        QFETCH(QString, error);
        QString got;
        QVERIFY(!McpIpcManifest::parse(json, &got));
        QCOMPARE(got, error);
    }

    void loadsAFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("mcp.json"));

        QString error;
        bool missing = false;
        std::optional<McpIpcManifest> manifest = McpIpcManifest::load(path, &error, &missing);
        QVERIFY(manifest);
        QVERIFY(missing);
        QVERIFY(manifest->tools.isEmpty());

        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"tools": [{"call": "a.b", "name": "n", "description": "d"}]})");
        file.close();
        manifest = McpIpcManifest::load(path, &error, &missing);
        QVERIFY(manifest);
        QVERIFY(!missing);
        QCOMPARE(manifest->tools.size(), 1);

        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(R"({"tools": 1})");
        file.close();
        QVERIFY(!McpIpcManifest::load(path, &error, &missing));
        QCOMPARE(error, QStringLiteral("%1: \"tools\" is not an array").arg(path));
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpIpcManifest)
#include "test_phosphor_mcp_ipc_manifest.moc"
