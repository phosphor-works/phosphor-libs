// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// The manifest's shape, which is strict: what it does not name is an error.

#include <PhosphorMcpIpc/McpIpcManifest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <optional>

#include <sys/stat.h>
#include <unistd.h>

using namespace PhosphorMcpIpc;

namespace {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

} // namespace

class TestPhosphorMcpIpcManifest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readsEveryMember()
    {
        QString error = QStringLiteral("stale");
        const std::optional<McpIpcManifest> manifest = McpIpcManifest::parse(R"({"tools": [
            {"call": "app.window.close", "name": "close_window", "title": "Close", "description": "Closes a window.",
             "params": ["window", "force"],
             "annotations": {"readOnly": false, "destructive": true, "idempotent": false, "openWorld": false}},
            {"call": "app.list", "name": "list", "description": "Lists.", "params": [], "annotations": {"readOnly": true}}
        ]})",
                                                                             &error);
        QVERIFY2(manifest, qPrintable(error));
        // A manifest that is read clears the error.
        QVERIFY(error.isEmpty());
        QCOMPARE(manifest->tools.size(), 2);
        QCOMPARE(manifest->problem(), QString());

        const McpIpcManifestTool& close = manifest->tools.at(0);
        // The target may hold dots, a method cannot.
        QCOMPARE(close.target, QStringLiteral("app.window"));
        QCOMPARE(close.method, QStringLiteral("close"));
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

        // The error may be null.
        QVERIFY(McpIpcManifest::parse(R"({"tools": []})").has_value());
        QVERIFY(!McpIpcManifest::parse("{").has_value());
    }

    void readsAnEmptyList()
    {
        QString error;
        const std::optional<McpIpcManifest> manifest = McpIpcManifest::parse(R"({"tools": []})", &error);
        QVERIFY2(manifest.has_value(), qPrintable(error));
        QVERIFY(manifest->tools.isEmpty());
        QCOMPARE(manifest->problem(), QString());
    }

    void refusesWhatItDoesNotName_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("error");
        // The text after the offset is Qt's own.
        QTest::newRow("not JSON") << QByteArray("{") << QStringLiteral("not JSON at offset 1: ");
        QTest::newRow("not an object") << QByteArray("[]") << QStringLiteral("the root is not an object");
        QTest::newRow("a number") << QByteArray("5") << QStringLiteral("the root is not an object");
        QTest::newRow("null") << QByteArray("null") << QStringLiteral("the root is not an object");
        QTest::newRow("no tools") << QByteArray("{}") << QStringLiteral("the root has no \"tools\"");
        QTest::newRow("tools not an array")
            << QByteArray(R"({"tools": {}})") << QStringLiteral("\"tools\" is not an array");
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
        QTest::newRow("call starting with a dot")
            << QByteArray(R"({"tools": [{"call": ".b", "name": "n", "description": "d"}]})")
            << QStringLiteral("tools[0]: \"call\" is not a string of the form TARGET.METHOD");
        QTest::newRow("call not a string")
            << QByteArray(R"({"tools": [{"call": 1, "name": "n", "description": "d"}]})")
            << QStringLiteral("tools[0]: \"call\" is not a string of the form TARGET.METHOD");
        QTest::newRow("no call") << QByteArray(R"({"tools": [{"name": "n", "description": "d"}]})")
                                 << QStringLiteral("tools[0]: there is no \"call\"");
        QTest::newRow("no name") << QByteArray(R"({"tools": [{"call": "a.b", "description": "d"}]})")
                                 << QStringLiteral("tools[0]: there is no \"name\"");
        QTest::newRow("name not a string")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": 1, "description": "d"}]})")
            << QStringLiteral("tools[0]: \"name\" is not a string");
        // The name rule of MCP, which the server would refuse only after
        // the socket answered, and only for a tool that the socket has.
        QTest::newRow("empty name") << QByteArray(R"({"tools": [{"call": "a.b", "name": "", "description": "d"}]})")
                                    << QStringLiteral(
                                           "tools[0]: \"name\" must be 1 to 128 of A-Z, a-z, 0-9, '_', '-' and '.'");
        QTest::newRow("name with a space")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "rename pane", "description": "d"}]})")
            << QStringLiteral("tools[0]: \"name\" must be 1 to 128 of A-Z, a-z, 0-9, '_', '-' and '.'");
        QTest::newRow("no description") << QByteArray(R"({"tools": [{"call": "a.b", "name": "n"}]})")
                                        << QStringLiteral("tools[0]: there is no \"description\"");
        QTest::newRow("description not a string")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": []}]})")
            << QStringLiteral("tools[0]: \"description\" is not a string");
        QTest::newRow("empty description")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": ""}]})")
            << QStringLiteral("tools[0]: \"description\" must not be empty");
        QTest::newRow("title not a string")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "title": 1}]})")
            << QStringLiteral("tools[0]: \"title\" is not a string");
        QTest::newRow("empty title") << QByteArray(
            R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "title": ""}]})")
                                     << QStringLiteral("tools[0]: \"title\" must not be empty");
        QTest::newRow("params not an array")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "params": "x"}]})")
            << QStringLiteral("tools[0]: \"params\" is not an array");
        QTest::newRow("params not strings")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "params": [1]}]})")
            << QStringLiteral("tools[0]: \"params\" holds a value that is not a string");
        QTest::newRow("params with an empty name")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "params": [""]}]})")
            << QStringLiteral("tools[0]: \"params\" holds an empty name");
        QTest::newRow("params twice") << QByteArray(
            R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "params": ["x", "x"]}]})")
                                      << QStringLiteral("tools[0]: \"params\" names \"x\" twice");
        QTest::newRow("annotations not an object")
            << QByteArray(R"({"tools": [{"call": "a.b", "name": "n", "description": "d", "annotations": true}]})")
            << QStringLiteral("tools[0]: \"annotations\" is not an object");
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
        if (error.endsWith(QLatin1StringView(": "))) {
            QVERIFY2(got.startsWith(error), qPrintable(got));
        } else {
            QCOMPARE(got, error);
        }
    }

    void checksAManifestThatCodeBuilt_data()
    {
        QTest::addColumn<QString>("target");
        QTest::addColumn<QString>("method");
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("description");
        QTest::addColumn<QStringList>("params");
        QTest::addColumn<QString>("problem");

        const QString t = QStringLiteral("t");
        const QString f = QStringLiteral("f");
        const QString n = QStringLiteral("n");
        const QString d = QStringLiteral("d");
        const QString call = QStringLiteral("tools[1]: \"call\" is not of the form TARGET.METHOD");
        QTest::newRow("valid") << t << f << n << d << QStringList{QStringLiteral("a")} << QString();
        QTest::newRow("no target") << QString() << f << n << d << QStringList() << call;
        QTest::newRow("no method") << t << QString() << n << d << QStringList() << call;
        QTest::newRow("a method with a dot") << t << QStringLiteral("a.b") << n << d << QStringList() << call;
        QTest::newRow("no name") << t << f << QString() << d << QStringList()
                                 << QStringLiteral(
                                        "tools[1]: \"name\" must be 1 to 128 of A-Z, a-z, 0-9, '_', '-' and '.'");
        QTest::newRow("no description") << t << f << n << QString() << QStringList()
                                        << QStringLiteral("tools[1]: \"description\" must not be empty");
        QTest::newRow("an empty parameter name")
            << t << f << n << d << QStringList{QString()} << QStringLiteral("tools[1]: \"params\" holds an empty name");
        QTest::newRow("a parameter two times")
            << t << f << n << d << QStringList{QStringLiteral("a"), QStringLiteral("a")}
            << QStringLiteral("tools[1]: \"params\" names \"a\" twice");
        QTest::newRow("the name of the tool before") << t << f << QStringLiteral("first") << d << QStringList()
                                                     << QStringLiteral("tools[1]: another tool is named \"first\"");
    }

    void checksAManifestThatCodeBuilt()
    {
        QFETCH(QString, target);
        QFETCH(QString, method);
        QFETCH(QString, name);
        QFETCH(QString, description);
        QFETCH(QStringList, params);
        QFETCH(QString, problem);

        McpIpcManifestTool first;
        first.target = QStringLiteral("t");
        first.method = QStringLiteral("g");
        first.name = QStringLiteral("first");
        first.description = QStringLiteral("d");
        McpIpcManifestTool second;
        second.target = target;
        second.method = method;
        second.name = name;
        second.description = description;
        second.params = params;
        McpIpcManifest manifest;
        manifest.tools = {first, second};
        QCOMPARE(manifest.problem(), problem);
    }

    void loadsAFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("mcp.json"));

        // No file is a manifest without tools.
        QString error = QStringLiteral("stale");
        bool missing = false;
        std::optional<McpIpcManifest> manifest = McpIpcManifest::load(path, &error, &missing);
        QVERIFY(manifest);
        QVERIFY(missing);
        QVERIFY(error.isEmpty());
        QVERIFY(manifest->tools.isEmpty());

        QVERIFY(writeFile(path, R"({"tools": [{"call": "a.b", "name": "n", "description": "d"}]})"));
        error = QStringLiteral("stale");
        manifest = McpIpcManifest::load(path, &error, &missing);
        QVERIFY2(manifest, qPrintable(error));
        QVERIFY(!missing);
        QVERIFY(error.isEmpty());
        QCOMPARE(manifest->tools.size(), 1);
        // `error` and `missing` may be null.
        QVERIFY(McpIpcManifest::load(path).has_value());

        QVERIFY(writeFile(path, R"({"tools": 1})"));
        QVERIFY(!McpIpcManifest::load(path, &error, &missing));
        QVERIFY(!missing);
        QCOMPARE(error, QStringLiteral("%1: \"tools\" is not an array").arg(path));
    }

    void refusesAPathThatIsNoManifest()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir directory(dir.path());
        QString error;
        bool missing = true;

        QVERIFY(!McpIpcManifest::load(QString(), &error, &missing));
        QVERIFY(!missing);
        QCOMPARE(error, QStringLiteral("the path of the manifest is empty"));

        // A directory.
        QVERIFY(!McpIpcManifest::load(dir.path(), &error, &missing));
        QVERIFY(!missing);
        QCOMPARE(error, QStringLiteral("%1 is not a regular file").arg(dir.path()));

        // A device.
        QVERIFY(!McpIpcManifest::load(QStringLiteral("/dev/null"), &error, &missing));
        QVERIFY(!missing);
        QCOMPARE(error, QStringLiteral("/dev/null is not a regular file"));

        // A FIFO. A plain open() of it would wait for a writer without end.
        const QString fifo = directory.filePath(QStringLiteral("fifo"));
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        QVERIFY(!McpIpcManifest::load(fifo, &error, &missing));
        QVERIFY(!missing);
        QCOMPARE(error, QStringLiteral("%1 is not a regular file").arg(fifo));

        // A link to no file: someone meant a manifest to be here.
        const QString link = directory.filePath(QStringLiteral("link.json"));
        QVERIFY(QFile::link(directory.filePath(QStringLiteral("gone.json")), link));
        QVERIFY(!McpIpcManifest::load(link, &error, &missing));
        QVERIFY(!missing);
        QCOMPARE(error, QStringLiteral("cannot read %1: it is a link to a file that does not exist").arg(link));

        // A link to a manifest is the manifest.
        const QString real = directory.filePath(QStringLiteral("real.json"));
        QVERIFY(writeFile(real, R"({"tools": []})"));
        const QString good = directory.filePath(QStringLiteral("good.json"));
        QVERIFY(QFile::link(real, good));
        QVERIFY2(McpIpcManifest::load(good, &error, &missing).has_value(), qPrintable(error));
        QVERIFY(!missing);

        // A file of more than 4 MiB.
        const QString large = directory.filePath(QStringLiteral("large.json"));
        QVERIFY(writeFile(large, QByteArray(4 * 1024 * 1024 + 1, ' ')));
        QVERIFY(!McpIpcManifest::load(large, &error, &missing));
        QCOMPARE(error, QStringLiteral("%1 is larger than 4194304 bytes").arg(large));
        // Exactly 4 MiB is read. It is all spaces, so it is no JSON.
        QVERIFY(writeFile(large, QByteArray(4 * 1024 * 1024, ' ')));
        QVERIFY(!McpIpcManifest::load(large, &error, &missing));
        QVERIFY2(error.startsWith(QStringLiteral("%1: not JSON at offset ").arg(large)), qPrintable(error));

        // A file that the user cannot read. Root reads every file.
        if (::geteuid() != 0) {
            const QString locked = directory.filePath(QStringLiteral("locked.json"));
            QVERIFY(writeFile(locked, R"({"tools": []})"));
            QVERIFY(QFile::setPermissions(locked, QFileDevice::Permissions()));
            QVERIFY(!McpIpcManifest::load(locked, &error, &missing));
            QVERIFY(!missing);
            QVERIFY2(error.startsWith(QStringLiteral("cannot read %1: ").arg(locked)), qPrintable(error));
        }
    }
};

QTEST_GUILESS_MAIN(TestPhosphorMcpIpcManifest)
#include "test_phosphor_mcp_ipc_manifest.moc"
