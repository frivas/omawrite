#include <QtTest>
#include <QFont>
#include <QFontMetricsF>
#include <QPrinter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTextDocument>

#include "backend.h"
#include "markdownhighlighter.h"
#include "systemtheme.h"

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_settingsDirectory.isValid());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDirectory.path());
    }

    void init() {
        QSettings().clear();
    }

    void countsWords() {
        QCOMPARE(Backend::countWords(QStringLiteral("one two-three don't 42")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好 世界")), 2);
        QCOMPARE(Backend::countWords(QString()), 0);
    }

    void normalizesLinks() {
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("www.example.com/path")),
                 QStringLiteral("https://www.example.com/path"));
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("mailto:writer@example.com")),
                 QStringLiteral("mailto:writer@example.com"));
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("example.com")).isEmpty());
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("file:///tmp/private")).isEmpty());
    }

    void suggestsSafeNames() {
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("My first draft\nBody")),
                 QStringLiteral("My first draft.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("A/B")), QStringLiteral("A-B.md"));
        QCOMPARE(Backend::suggestedFileName(QString()), QStringLiteral("Untitled.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("Already.md")),
                 QStringLiteral("Already.md"));
    }

    void detectsSystemAppearanceWithoutRequiringAPortal() {
        SystemTheme theme;
        QVERIFY(theme.textScale() > 0.0);
    }

    void findsInlineMarkdownRanges() {
        const auto markup = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic* and [site](https://example.com)"));
        QCOMPARE(markup.size(), 3);
        QCOMPARE(markup.at(0).content.start, 2);
        QCOMPARE(markup.at(0).content.length, 4);
        QCOMPARE(markup.at(2).content.length, 4);
        QCOMPARE(markup.at(2).markers[0].length, 1);
    }

    void loadsCurrentOmarchyTheme() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());

        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        const QString themeDirectory = homeDirectory.path()
            + QStringLiteral("/.local/state/omarchy/current/theme");
        QVERIFY(QDir().mkpath(themeDirectory));

        QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
        QVERIFY(colorsFile.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray palette(
            "mode = \"light\"\n"
            "accent = \"#112233\"\n"
            "selection = \"#445566\"\n"
            "background = \"#fefefe\"\n"
            "foreground = \"#101010\"\n");
        QCOMPARE(colorsFile.write(palette), qint64(palette.size()));
        colorsFile.close();

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#fefefe"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#101010"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QVERIFY(!backend.darkMode());
    }

    void ignoresFileWatcherEventsForSavedContents() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("first-save.md"));
        Backend backend;
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        backend.saveAs(QUrl::fromLocalFile(path));
        QVERIFY(QFileInfo::exists(path));

        QFile sameContents(path);
        QVERIFY(sameContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        sameContents.close();
        QTest::qWait(100);
        QCOMPARE(externalChangeSpy.count(), 0);

        QFile changedContents(path);
        QVERIFY(changedContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(changedContents.write("changed elsewhere"), qint64(17));
        changedContents.close();
        QTRY_COMPARE(externalChangeSpy.count(), 1);
    }

    void keepsCursorAndSelectionStableAcrossInsertions() {
        const QString mutationsPath = QFINDTESTDATA("../src/EditorMutations.js");
        QVERIFY(!mutationsPath.isEmpty());

        QQmlEngine engine;
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import "EditorMutations.js" as EditorMutations

            TextEdit {
                property string insertionText
                property int insertionCursor
                property string wrappedText
                property int wrappedSelectionStart
                property int wrappedSelectionEnd

                Component.onCompleted: {
                    text = "alpha omega";
                    cursorPosition = 5;
                    EditorMutations.replaceRange(this, 5, 5, "one\r\ntwo");
                    insertionText = text;
                    insertionCursor = cursorPosition;

                    text = "alpha beta omega";
                    select(6, 10);
                    EditorMutations.replaceRange(this, selectionStart, selectionEnd,
                                                 "**beta**", 2, 6);
                    wrappedText = text;
                    wrappedSelectionStart = selectionStart;
                    wrappedSelectionEnd = selectionEnd;
                }
            }
        )QML";
        const QUrl harnessUrl = QUrl::fromLocalFile(
            QFileInfo(mutationsPath).absolutePath() + QStringLiteral("/MutationHarness.qml"));
        component.setData(harness, harnessUrl);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        QCOMPARE(editor->property("insertionText").toString(),
                 QStringLiteral("alphaone\ntwo omega"));
        QCOMPARE(editor->property("insertionCursor").toInt(), 12);
        QCOMPARE(editor->property("wrappedText").toString(),
                 QStringLiteral("alpha **beta** omega"));
        QCOMPARE(editor->property("wrappedSelectionStart").toInt(), 8);
        QCOMPARE(editor->property("wrappedSelectionEnd").toInt(), 12);
    }

    void startsANewFileFromAPathThatIsNotThereYet() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString newPath = directory.filePath(QStringLiteral("new_document.md"));
        const QString existingPath = directory.filePath(QStringLiteral("already-there.md"));
        QFile existing(existingPath);
        QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Text));
        existing.write("on disk already");
        existing.close();

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // A name from the command line that is not on disk yet is still this
        // document's name, blank as the document is.
        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        backend.open(QUrl::fromLocalFile(newPath));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(newPath));
        QCOMPARE(backend.fileName(), QStringLiteral("new_document.md"));
        QCOMPARE(backend.status(), QStringLiteral("New file new_document.md"));
        QCOMPARE(editor->property("text").toString(), QString());
        QVERIFY(!backend.modified());

        // Opening it wrote nothing: the file appears when the writer saves.
        QVERIFY(!QFileInfo::exists(newPath));

        editor->setProperty("text", QStringLiteral("first words"));
        QVERIFY(backend.modified());
        backend.save();
        QCOMPARE(saveDialogSpy.count(), 0);
        QVERIFY(!backend.modified());

        QFile written(newPath);
        QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(written.readAll(), QByteArray("first words"));
        written.close();

        // A file that is there still opens and reads.
        backend.open(QUrl::fromLocalFile(existingPath));
        QCOMPARE(backend.fileName(), QStringLiteral("already-there.md"));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("on disk already"));

        // A path that is there but cannot be read is still an error, and
        // leaves the document it could not replace alone.
        backend.open(QUrl::fromLocalFile(directory.path()));
        QCOMPARE(backend.status(),
                 QStringLiteral("Could not open %1.")
                     .arg(QFileInfo(directory.path()).fileName()));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(existingPath));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("on disk already"));

        // A name under a directory that is not there is not a file anyone can
        // start, so it stays an error rather than a document that cannot save.
        backend.open(QUrl::fromLocalFile(
            directory.filePath(QStringLiteral("not-there/child.md"))));
        QCOMPARE(backend.status(), QStringLiteral("Could not open child.md."));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(existingPath));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("on disk already"));
    }

    void asksBeforeAFirstSaveReplacesAFileThatAppeared() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("arriving.md"));

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(backend.status(), QStringLiteral("New file arriving.md"));
        editor->setProperty("text", QStringLiteral("my draft"));
        QVERIFY(backend.modified());

        // A file that is not there cannot be watched, so nothing tells us when
        // a `git pull` or a sync client puts one on that path. The first save
        // is the first look, and it must not replace a file it has never read.
        QFile arrived(path);
        QVERIFY(arrived.open(QIODevice::WriteOnly | QIODevice::Text));
        arrived.write("arrived from elsewhere");
        arrived.close();

        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
        QCOMPARE(appearedSpy.takeFirst().constFirst().toBool(), true);

        // Asked, not answered: the file on disk is whole and the draft is
        // still unsaved. The name is not in question, so no Save As dialog.
        QCOMPARE(saveDialogSpy.count(), 0);
        QVERIFY(backend.modified());
        QFile untouched(path);
        QVERIFY(untouched.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(untouched.readAll(), QByteArray("arrived from elsewhere"));
        untouched.close();

        // Keeping your version is what the dialog offers, and the save that
        // follows it goes through: the guard asks once, it does not lock the
        // writer out of the name they gave.
        backend.keepExternalVersion();
        backend.save();
        QVERIFY(!backend.modified());
        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(written.readAll(), QByteArray("my draft"));
        written.close();
    }

    void asksOnlyOnceWhenWhatAppearedCannotBeRead() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("blocked.md"));

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("my draft"));
        QVERIFY(backend.modified());

        // What turns up on the path need not be a readable file. A directory
        // is the plainest case: keepExternalVersion() cannot read it, so it
        // has no contents to remember afterwards.
        QVERIFY(QDir().mkpath(path));

        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);

        // Keeping your version answers the question, and an answer that could
        // not be read is still an answer. Asking again would put the writer in
        // a dialog with no way out of it, every Ctrl+S for the rest of the
        // session. The second save goes to the filesystem and reports what the
        // filesystem says, which is the only thing that can end this.
        backend.keepExternalVersion();
        QCOMPARE(backend.status(), QStringLiteral("Kept your version"));
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
        QCOMPARE(backend.status(), QStringLiteral("Could not save blocked.md."));
    }

    void dropsThePendingCloseWhenTheSaveIsRefused() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("closing.md"));

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("my draft"));
        QVERIFY(backend.modified());

        QFile arrived(path);
        QVERIFY(arrived.open(QIODevice::WriteOnly | QIODevice::Text));
        arrived.write("arrived from elsewhere");
        arrived.close();

        // Closing the window with unsaved changes leaves "close" standing
        // while the unsaved-changes dialog's Save runs. The guard turns that
        // save into a question, so the close it was for cannot follow.
        window->setProperty("pendingAction", QStringLiteral("close"));
        window->setProperty("awaitingPendingSave", true);
        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
        QCOMPARE(window->property("pendingAction").toString(), QString());
        QVERIFY(!window->property("awaitingPendingSave").toBool());

        // Otherwise the next successful save -- this one, minutes later and
        // asked for on its own -- closes the window on the earlier request.
        backend.keepExternalVersion();
        backend.save();
        QVERIFY(!backend.modified());
        QVERIFY(!window->property("closeConfirmed").toBool());
    }

    void putsKeepMineForwardWhenAFileAppeared() {
        const QString dialogPath = QFINDTESTDATA("../src/ExternalChangeDialog.qml");
        QVERIFY(!dialogPath.isEmpty());

        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(dialogPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> dialog(component.create());
        QVERIFY2(dialog, qPrintable(component.errorString()));

        QObject *keep = dialog->findChild<QObject *>(QStringLiteral("keepMineButton"));
        QObject *reload = dialog->findChild<QObject *>(QStringLiteral("reloadButton"));
        QObject *message = dialog->findChild<QObject *>(QStringLiteral("externalChangeMessage"));
        QObject *heading = dialog->findChild<QObject *>(QStringLiteral("externalChangeHeading"));
        QVERIFY(keep);
        QVERIFY(reload);
        QVERIFY(message);
        QVERIFY(heading);

        // For an ordinary outside edit the file on disk is a second copy of
        // the work, so Reload is the safe answer and leads, as it always has.
        QVERIFY(!dialog->property("keepIsSafer").toBool());
        QVERIFY(reload->property("primary").toBool());
        QVERIFY(!keep->property("primary").toBool());

        // For a file that appeared there is no second copy: every word the
        // writer has is in the editor, and reloading throws all of it away,
        // recovery snapshot included. The button that does that must not be
        // the one Enter presses, and the text must say what is at stake.
        dialog->setProperty("appeared", true);
        QVERIFY(dialog->property("keepIsSafer").toBool());
        QVERIFY(keep->property("primary").toBool());
        QVERIFY(!reload->property("primary").toBool());
        QCOMPARE(heading->property("text").toString(), QStringLiteral("File appeared"));
        const QString message_ = message->property("text").toString();
        QVERIFY2(message_.contains(QStringLiteral("created this file")), qPrintable(message_));
        QVERIFY2(message_.contains(QStringLiteral("discard everything")), qPrintable(message_));
    }

    void keepsTheDocumentWhenReloadRacesADeletion() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("racing.md"));
        QFile onDisk(path);
        QVERIFY(onDisk.open(QIODevice::WriteOnly | QIODevice::Text));
        onDisk.write("what was there");
        onDisk.close();

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("what was there"));
        editor->setProperty("text", QStringLiteral("words only I have"));
        QVERIFY(backend.modified());

        // The "File changed" dialog leaves Reload enabled for a file that was
        // still there when it opened. If the file goes away before the click,
        // the reload has nothing to read: it must say so, not take the missing
        // path for a new document and blank the only copy of this text.
        QVERIFY(QFile::remove(path));
        backend.reloadFromDisk();
        QCOMPARE(backend.status(), QStringLiteral("Could not open racing.md."));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("words only I have"));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(path));
        QVERIFY(backend.modified());

        // Refusing the reload leaves this document where a new file starts:
        // a name, nothing behind it, and a watcher that let the path go when
        // the file did. So the same thing can happen again from here -- the
        // pull that removed the file landing the next commit -- and the save
        // has to ask about it just the same.
        QFile returned(path);
        QVERIFY(returned.open(QIODevice::WriteOnly | QIODevice::Text));
        returned.write("came back different");
        returned.close();

        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
        QFile intact(path);
        QVERIFY(intact.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(intact.readAll(), QByteArray("came back different"));
        intact.close();
    }

    void writesTheNeverReadPathIntoTheSnapshot() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("fresh.md"));

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("words only I have"));
        QVERIFY(backend.modified());

        // The snapshot the next run reads is the one this run wrote, so the
        // flag has to survive the write as well as the read. Hand-writing the
        // JSON proves only half of that, and it is the half that cannot lose
        // a file.
        const QString snapshotPath =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("recovery-0.json"));
        QTRY_VERIFY(QFile::exists(snapshotPath));

        QFile snapshot(snapshotPath);
        QVERIFY(snapshot.open(QIODevice::ReadOnly));
        const QJsonObject recovery = QJsonDocument::fromJson(snapshot.readAll()).object();
        snapshot.close();
        QVERIFY(recovery.contains(QStringLiteral("pathNeverRead")));
        QVERIFY(recovery.value(QStringLiteral("pathNeverRead")).toBool());
    }

    void remembersANeverReadPathAcrossRecovery() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());
        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("fresh.md"));

        // The snapshot a crash leaves behind, for a new file whose first save
        // never happened.
        const QString stateDirectory =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(stateDirectory));
        QFile snapshot(QDir(stateDirectory).filePath(QStringLiteral("recovery-0.json")));
        QVERIFY(snapshot.open(QIODevice::WriteOnly));
        const QJsonObject recovery{
            {QStringLiteral("fileUrl"), QUrl::fromLocalFile(path).toString()},
            {QStringLiteral("pathNeverRead"), true},
            {QStringLiteral("text"), QStringLiteral("words only I have")}};
        snapshot.write(QJsonDocument(recovery).toJson(QJsonDocument::Compact));
        snapshot.close();

        // A file turns up on the path while Omawrite is not running to see it.
        QFile arrived(path);
        QVERIFY(arrived.open(QIODevice::WriteOnly | QIODevice::Text));
        arrived.write("arrived while we were down");
        arrived.close();

        // Reading it back on restore says what is on the path now, which is
        // not the same as this document having read it. Without the flag the
        // first save takes the guard's silence for permission.
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QCOMPARE(backend.status(), QStringLiteral("Recovered unsaved changes"));
        QSignalSpy appearedSpy(&backend, &Backend::externalFileAppeared);
        backend.save();
        QCOMPARE(appearedSpy.count(), 1);
        QFile untouched(path);
        QVERIFY(untouched.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(untouched.readAll(), QByteArray("arrived while we were down"));
        untouched.close();
    }

    void savesAndOpensFromFooterButtons() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QVERIFY(window->findChild<QObject *>(QStringLiteral("sourceEditor")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("renderedPreview")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("modeToggle")));

        QObject *saveButton = window->findChild<QObject *>(QStringLiteral("saveButton"));
        QObject *openButton = window->findChild<QObject *>(QStringLiteral("openButton"));
        QVERIFY(saveButton);
        QVERIFY(openButton);

        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(saveButton, "clicked"));
        QCOMPARE(saveDialogSpy.count(), 1);

        QSignalSpy openDialogSpy(&backend, &Backend::openDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(openButton, "clicked"));
        QCOMPARE(openDialogSpy.count(), 1);
    }

    void breaksLinesOnReturn() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto load = [&](const QString &content, int position) {
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };
        auto returnKey = [&] { QTest::keyClick(window, Qt::Key_Return); };

        // Ending a paragraph leaves the blank line that separates it from the
        // next one.
        load(QStringLiteral("one\n\ntwo"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n\n"));

        // On a line that is already blank there is nothing to separate from,
        // so Return is worth one line, not two.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));

        // An empty document is a blank line too.
        load(QString(), 0);
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n"));

        // Whitespace left behind on a line still reads as blank.
        load(QStringLiteral("one\n  \ntwo"), 5);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n \n \ntwo"));

        // A selection dragged right to left leaves the caret on the blank line
        // it began on, but the break lands on the line it leaves behind.
        load(QStringLiteral("one\n\ntwo"), 7);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 4));
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\no"));

        // A list carries its marker down, and an item left empty drops the
        // marker to end the list.
        load(QStringLiteral("- item"), 6);
        returnKey();
        QCOMPARE(text(), QStringLiteral("- item\n- "));
        returnKey();
        QCOMPARE(text(), QStringLiteral("- item\n\n"));

        // Inside a code fence every line is its own, blank ones included.
        load(QStringLiteral("```\ncode\n```"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("```\ncode\n\n```"));
        returnKey();
        QCOMPARE(text(), QStringLiteral("```\ncode\n\n\n```"));
    }

    void closesParagraphBreaksOnBackspace() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto caret = [&] { return editor->property("cursorPosition").toInt(); };
        auto load = [&](const QString &content, int position) {
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };
        auto returnKey = [&] { QTest::keyClick(window, Qt::Key_Return); };
        auto backspaceKey = [&] { QTest::keyClick(window, Qt::Key_Backspace); };

        // Backspace at the head of a paragraph closes the whole break above
        // it, so the two paragraphs join in one press rather than two, and
        // the caret lands where they meet.
        load(QStringLiteral("one\n\ntwo"), 5);
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("onetwo"));
        QCOMPARE(caret(), 3);

        // Ending a paragraph writes both breaks at once, and one Backspace
        // takes both back. A Return on the blank line of "one\n\n\ntwo" can
        // leave this very document with the caret in this very place, so it
        // is not provenance that decides here: it is that the gap is left
        // standing either way, and this is much the commoner press.
        load(QStringLiteral("one\n\ntwo"), 3);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));
        QCOMPARE(caret(), 3);

        // Which is what a Return inside a wide gap costs: the same rule reads
        // this as the paragraph-ending press it cannot be told apart from, so
        // the writer gets back one blank line fewer than they had. The gap
        // still separates the paragraphs, and one more Return returns it.
        load(QStringLiteral("one\n\n\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        QCOMPARE(caret(), 3);

        // Return on a blank line writes a single break, so Backspace must
        // take back a single break too. Closing the pair behind the caret
        // would swallow the separator that was there beforehand and leave
        // the two paragraphs with nothing between them.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // The same holds however far the gap has grown: each Return is worth
        // one Backspace.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));
        backspaceKey();
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // A line with nothing but spaces on it is a blank line, and Return
        // above one writes a single break there too.
        load(QStringLiteral("one\n  \ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n  \ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n  \ntwo"));

        // On the blank line a document ends with, the two Returns leave the
        // same text behind and no reading of it says which was pressed.
        // Backspace takes one break, because taking two would carry off the
        // line the document already ended with.
        load(QStringLiteral("one\n"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n"));

        // Which costs the paragraph that ends a document a second press,
        // and that is the whole of what it costs: nothing is lost on the
        // way through.
        load(QStringLiteral("one\n\ntwo"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // An empty document is a blank line, so it takes two Returns to open
        // a gap and two Backspaces to close it again.
        load(QString(), 0);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("\n"));
        backspaceKey();
        QCOMPARE(text(), QString());

        // A document holding nothing but spaces is a blank line as well, and
        // the spaces are text: they must still be there at the end of it.
        load(QStringLiteral("   "), 3);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("   \n\n"));
        backspaceKey();
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("   "));

        // Return at the very head of a document writes both breaks, since
        // the paragraph below it is not blank, and one Backspace takes them.
        load(QStringLiteral("two"), 0);
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("two"));
        QCOMPARE(caret(), 0);

        // Spaces left on the line above a break do not make the break any
        // less of one: Return between them and the paragraph below wrote
        // both of these, so Backspace still takes both.
        load(QStringLiteral("one\n  \n\ntwo"), 8);
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n  two"));
        QCOMPARE(caret(), 6);
    }

    void scalesTextWithDesktopTextSize() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 20);

        // `omarchy display text size 16` sets the GNOME factor to 16/12.
        backend.setTextScale(16.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 27);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 27);

        backend.setTextScale(9.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 15);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 15);
    }

    void dispatchesEditorFontSizeShortcuts() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        window->requestActivate();
        QTRY_VERIFY(window->isActive());

        QCOMPARE(backend.editorFontSize(), 20);
        QTest::keyClick(window, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(backend.editorFontSize(), 22);
        QTest::keyClick(window, Qt::Key_Plus, Qt::ControlModifier);
        QCOMPARE(backend.editorFontSize(), 24);
        QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(backend.editorFontSize(), 22);
        QTest::keyClick(window, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(backend.editorFontSize(), 20);
        QCOMPARE(QSettings().value(QStringLiteral("editor/fontSize")).toInt(), 20);
    }

    void persistsEditorFontSizeAcrossBackendInstances() {
        {
            Backend backend;
            QCOMPARE(backend.editorFontSize(), 20);

            QSignalSpy changedSpy(&backend, &Backend::editorFontSizeChanged);
            backend.setEditorFontSize(28);
            QCOMPARE(changedSpy.count(), 1);
            QCOMPARE(QSettings().value(QStringLiteral("editor/fontSize")).toInt(), 28);
        }

        Backend restoredBackend;
        QCOMPARE(restoredBackend.editorFontSize(), 28);

        restoredBackend.setEditorFontSize(100);
        QCOMPARE(restoredBackend.editorFontSize(), 48);
        restoredBackend.setEditorFontSize(0);
        QCOMPARE(restoredBackend.editorFontSize(), 10);
    }

    // Issue #29: the editor sizes its font in pixels, and a HighResolution
    // QPrinter reads those as 1200-dpi dots, printing text about 0.4 mm tall.
    void convertsPixelFontsToPointsForPrinting() {
        QFont editorFont(QStringLiteral("iA Writer Mono S"));
        editorFont.setPixelSize(18);

        const QFont printed = Backend::printFont(editorFont, 96.0);
        QCOMPARE(printed.pixelSize(), -1);
        QVERIFY(qAbs(printed.pointSizeF() - 13.5) < 0.01);

        // An unusable DPI falls back to 96 rather than producing a zero size.
        QVERIFY(qAbs(Backend::printFont(editorFont, 0.0).pointSizeF() - 13.5) < 0.01);

        // A font that already carries a point size is left alone.
        QFont pointFont(QStringLiteral("iA Writer Mono S"));
        pointFont.setPointSizeF(11.0);
        QCOMPARE(Backend::printFont(pointFont, 96.0).pointSizeF(), 11.0);
    }

    void printsTextAtALegibleSizeOnAHighResolutionPrinter() {
        QTemporaryDir outputDirectory;
        QVERIFY(outputDirectory.isValid());

        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(outputDirectory.filePath(QStringLiteral("out.pdf")));

        QFont editorFont(QStringLiteral("iA Writer Mono S"));
        editorFont.setPixelSize(18);

        // Measured on the printer itself: at 1200 dpi the unconverted font
        // lays out around 18 dots. A legible line is a good deal more than a
        // tenth of an inch.
        const QFontMetricsF printedMetrics(Backend::printFont(editorFont, 96.0), &printer);
        QVERIFY2(printedMetrics.height() > printer.logicalDpiY() * 0.1,
                 qPrintable(QStringLiteral("line height was %1 dots at %2 dpi")
                                .arg(printedMetrics.height())
                                .arg(printer.logicalDpiY())));

        const QFontMetricsF rawMetrics(editorFont, &printer);
        QVERIFY(printedMetrics.height() > rawMetrics.height() * 4);
    }

    // Issue #23: a save that does not happen used to leave pendingAction at
    // "close", so the next successful save closed the window.
    void dropsThePendingCloseWhenASaveFails() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        QTemporaryDir documentDirectory;
        QVERIFY(documentDirectory.isValid());
        const QString documentPath = documentDirectory.filePath(QStringLiteral("doc.md"));
        {
            QFile seed(documentPath);
            QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Text));
            seed.write("draft\n");
        }

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        backend.open(QUrl::fromLocalFile(documentPath));
        QCOMPARE(backend.fileUrl(), QUrl::fromLocalFile(documentPath));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("draft plus more"));
        QVERIFY(backend.modified());

        // Put a directory where the file was, so the save fails for any uid.
        QVERIFY(QFile::remove(documentPath));
        QVERIFY(QDir().mkpath(documentPath));

        // What the unsaved-changes dialog's Save button does.
        window->setProperty("pendingAction", QStringLiteral("close"));
        window->setProperty("awaitingPendingSave", true);
        backend.save();

        QVERIFY(backend.status().startsWith(QStringLiteral("Could not")));
        QCOMPARE(window->property("pendingAction").toString(), QString());
        QCOMPARE(window->property("awaitingPendingSave").toBool(), false);
        QCOMPARE(window->property("closeConfirmed").toBool(), false);

        // An ordinary save minutes later must not carry out that close.
        QVERIFY(QDir().rmdir(documentPath));
        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);
        backend.save();
        QCOMPARE(savedSpy.count(), 1);
        QCOMPARE(window->property("closeConfirmed").toBool(), false);
    }

    void findsTheAppBundleAroundAMacExecutable() {
        QCOMPARE(Backend::enclosingBundlePath(
                     QStringLiteral("/Applications/Omawrite.app/Contents/MacOS")),
                 QStringLiteral("/Applications/Omawrite.app"));

        // A bare executable, as on Linux or in an unbundled test build.
        QVERIFY(Backend::enclosingBundlePath(QStringLiteral("/usr/bin")).isEmpty());
        QVERIFY(Backend::enclosingBundlePath(QStringLiteral("/tmp/build")).isEmpty());
        // The right layout, but the top directory is not a bundle.
        QVERIFY(Backend::enclosingBundlePath(
                    QStringLiteral("/tmp/omawrite/Contents/MacOS")).isEmpty());
    }

    void remembersLastSaveDirectory() {
        QTemporaryDir saveDirectory;
        QVERIFY(saveDirectory.isValid());

        const QString savedPath = saveDirectory.filePath(QStringLiteral("first.md"));
        Backend savedDocument;
        savedDocument.saveAs(QUrl::fromLocalFile(savedPath));

        Backend nextDocument;
        QSignalSpy saveDialogSpy(&nextDocument, &Backend::saveDialogRequested);
        nextDocument.saveAsDialog();
        QCOMPARE(saveDialogSpy.count(), 1);

        const QUrl suggestedUrl = saveDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).absolutePath(),
                 saveDirectory.path());
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).fileName(),
                 QStringLiteral("Untitled.md"));

        QSettings().setValue(QStringLiteral("file/lastSaveDirectory"),
                             saveDirectory.filePath(QStringLiteral("missing")));
        Backend fallbackDocument;
        QSignalSpy fallbackDialogSpy(&fallbackDocument, &Backend::saveDialogRequested);
        fallbackDocument.saveAsDialog();
        const QUrl fallbackUrl = fallbackDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(fallbackUrl.toLocalFile()).absolutePath(), QDir::homePath());
    }

    void revertsEmptyDocumentToCleanState() {
        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        QVERIFY(!backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        quickDocument->textDocument()->setPlainText(QString());
        QVERIFY(backend.editorTextChanged());
        QVERIFY(!backend.modified());
    }

    void revertsSavedDocumentToCleanState() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("doc.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(file.write("hello"), qint64(5));
        file.close();

        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        backend.open(QUrl::fromLocalFile(path));
        QVERIFY(!backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello world"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(!backend.modified());
    }

    void keepsADocumentUnsavedAfterItsFileDisappears() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("gone.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(file.write("hello"), qint64(5));
        file.close();

        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);
        backend.open(QUrl::fromLocalFile(path));
        QVERIFY(!backend.modified());

        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(externalChangeSpy.count(), 1);

        // The writer can dismiss the dialog without picking a version, so the
        // text the deleted file used to hold is not a saved state to return to.
        quickDocument->textDocument()->setPlainText(QStringLiteral("hello world"));
        QVERIFY(backend.editorTextChanged());
        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        // Nor is emptying it: an empty document only reads as clean while there
        // is no file behind it.
        quickDocument->textDocument()->setPlainText(QString());
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
    }

private:
    // Loads a bare TextEdit, attaches its document to the backend, and returns
    // the QQuickTextDocument. The engine and TextEdit are parented to the
    // backend so they outlive the helper call.
    QQuickTextDocument *attachTextEdit(Backend *backend) {
        auto *engine = new QQmlEngine(backend);
        auto *component = new QQmlComponent(engine);
        component->setData(R"QML(
            import QtQuick
            TextEdit { }
        )QML", QUrl::fromLocalFile(QStringLiteral("Harness.qml")));
        if (!component->isReady())
            return nullptr;

        QObject *editor = component->create();
        if (!editor)
            return nullptr;
        editor->setParent(backend);

        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        if (!quickDocument)
            return nullptr;

        backend->attachDocument(quickDocument);
        return quickDocument;
    }

    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"
