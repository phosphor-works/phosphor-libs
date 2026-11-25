// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorPaths/TreeMove.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

namespace {
void writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
} // namespace

class TestTreeMove : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void movesEveryFileAndRemovesTheEmptiedTree()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString from = root.filePath(QStringLiteral("old"));
        const QString to = root.filePath(QStringLiteral("new/app"));
        writeFile(from + QStringLiteral("/config.json"), "config");
        writeFile(from + QStringLiteral("/themes/dusk.json"), "dusk");
        writeFile(from + QStringLiteral("/.hidden"), "hidden");
        QDir().mkpath(from + QStringLiteral("/empty/deeper"));

        PhosphorPaths::moveTreeContents(from, to);

        QCOMPARE(readFile(to + QStringLiteral("/config.json")), QByteArrayLiteral("config"));
        QCOMPARE(readFile(to + QStringLiteral("/themes/dusk.json")), QByteArrayLiteral("dusk"));
        QCOMPARE(readFile(to + QStringLiteral("/.hidden")), QByteArrayLiteral("hidden"));
        QVERIFY(!QFileInfo::exists(from));
    }

    void leavesAFileTheNewPlaceAlreadyHas()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString from = root.filePath(QStringLiteral("old"));
        const QString to = root.filePath(QStringLiteral("new"));
        writeFile(from + QStringLiteral("/config.json"), "old");
        writeFile(from + QStringLiteral("/other.json"), "other");
        writeFile(to + QStringLiteral("/config.json"), "new");

        PhosphorPaths::moveTreeContents(from, to);

        QCOMPARE(readFile(to + QStringLiteral("/config.json")), QByteArrayLiteral("new"));
        QCOMPARE(readFile(to + QStringLiteral("/other.json")), QByteArrayLiteral("other"));
        // The source that was not moved keeps its folder.
        QCOMPARE(readFile(from + QStringLiteral("/config.json")), QByteArrayLiteral("old"));
        QVERIFY(!QFileInfo::exists(from + QStringLiteral("/other.json")));
    }

    void doesNothingWithoutASourceTree()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString to = root.filePath(QStringLiteral("new"));
        PhosphorPaths::moveTreeContents(root.filePath(QStringLiteral("missing")), to);
        PhosphorPaths::moveTreeContents(QString(), to);
        QVERIFY(!QFileInfo::exists(to));
    }
};

QTEST_GUILESS_MAIN(TestTreeMove)
#include "test_tree_move.moc"
