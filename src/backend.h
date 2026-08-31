#pragma once

#include <QFont>
#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QFileSystemWatcher>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

class MarkdownHighlighter;
class QTextDocument;
class QWindow;
class QLockFile;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileUrlChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileUrlChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY wordCountChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal textScale READ textScale WRITE setTextScale NOTIFY textScaleChanged)
    Q_PROPERTY(int editorFontSize READ editorFontSize WRITE setEditorFontSize
               NOTIFY editorFontSizeChanged)
    Q_PROPERTY(bool autosave READ autosave WRITE setAutosave NOTIFY autosaveChanged)
    Q_PROPERTY(int autosaveDelayMs READ autosaveDelayMs WRITE setAutosaveDelayMs
               NOTIFY autosaveDelayMsChanged)
    Q_PROPERTY(QString themeBackground READ themeBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeForeground READ themeForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelection READ themeSelection NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeCodeBackground READ themeCodeBackground NOTIFY themeColorsChanged)

public:
    // Converts a pixel-sized editor font into the resolution-independent
    // point size a QPrinter needs. Exposed for testing.
    static QFont printFont(const QFont &editorFont, qreal screenDpi);

    // The name a print job carries. macOS offers it as the default filename in
    // Save as PDF, so it has to be the document's name without the Markdown
    // extension rather than the app's idea of an untitled job.
    static QString printJobName(const QString &documentFileName);

    // Starts a second Omawrite, optionally on a file. Exposed so main.cpp can
    // reuse it for Finder's open-document events.
    static bool launchNewInstance(const QString &filePath = QString());

    // The .app enclosing an executable directory, or an empty string when the
    // executable is not inside a bundle. Takes the directory rather than
    // reading it from the application so it can be tested off a real bundle.
    static QString enclosingBundlePath(const QString &executableDirPath);

    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    void setParentWindow(QWindow *window);

    QUrl fileUrl() const { return m_fileUrl; }
    QString fileName() const;

    bool modified() const { return m_modified; }
    QString status() const { return m_status; }
    int wordCount() const { return m_wordCount; }
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool darkMode);
    qreal textScale() const { return m_textScale; }
    void setTextScale(qreal textScale);
    int editorFontSize() const { return m_editorFontSize; }
    void setEditorFontSize(int editorFontSize);
    bool autosave() const { return m_autosave; }
    void setAutosave(bool autosave);
    int autosaveDelayMs() const { return m_autosaveDelayMs; }
    void setAutosaveDelayMs(int autosaveDelayMs);

    // Whether the debounce may write the document's own file, rather than the
    // recovery snapshot. Exposed so the guardrails can be tested directly.
    bool canAutosaveToFile() const;

    // Where this window's crash snapshot lives. Public so a test can read the
    // snapshot from the slot this backend actually claimed.
    QString recoveryPath() const;
    QString themeBackground() const { return m_themeBackground; }
    QString themeForeground() const { return m_themeForeground; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeSelection() const { return m_themeSelection; }
    QString themeCodeBackground() const { return m_themeCodeBackground; }
    static int countWords(const QString &text);
    static QString normalizedLinkUrl(const QString &clipboardText);
    static QString suggestedFileName(const QString &text);
    static QString tomlValue(const QString &text);

    Q_INVOKABLE void attachDocument(QObject *textDocument);
    Q_INVOKABLE void openDialog();
    Q_INVOKABLE void open(const QUrl &url);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveForClose();
    Q_INVOKABLE void saveAsDialog();
    Q_INVOKABLE void saveAs(const QUrl &url);
    Q_INVOKABLE void fileDialogCanceled();
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void reloadFromDisk();
    Q_INVOKABLE void keepExternalVersion();
    Q_INVOKABLE void resetEditorFontSize();
    Q_INVOKABLE void printDocument();
    Q_INVOKABLE void newWindow();
    Q_INVOKABLE QString clipboardUrl() const;
    Q_INVOKABLE QString clipboardText() const;
    Q_INVOKABLE bool editorTextChanged();
    Q_INVOKABLE QVariantList hiddenRangesAt(int position) const;
    Q_INVOKABLE void setSearchHighlight(const QString &query, int currentMatchStart);
    Q_INVOKABLE void openExternalUrl(const QUrl &url);
    Q_INVOKABLE QVariantMap windowGeometry() const;
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height, bool maximized);

signals:
    void fileUrlChanged();
    void modifiedChanged();
    void statusChanged();
    void wordCountChanged();
    void darkModeChanged();
    void textScaleChanged();
    void editorFontSizeChanged();
    void autosaveChanged();
    void autosaveDelayMsChanged();
    void themeColorsChanged();
    void closeAfterSave();
    void openDialogRequested();
    void saveDialogRequested(const QUrl &suggestedUrl);
    void saveSucceeded();
    void saveFailed();
    void externalChangeDetected(bool deleted, bool locallyModified);
    void externalFileAppeared(bool locallyModified);

private:
    void openPath(const QUrl &url, bool mayStartNewFile);
    void loadDocumentText(const QString &text);
    void setFileUrl(const QUrl &url);
    void setModified(bool modified);
    void setStatus(const QString &status);
    void saveTo(const QUrl &url);
    QUrl suggestedSaveUrl() const;
    QString currentDocumentText() const;
    void setWordCount(int words);
    void refreshWordCount();
    void scheduleWordCount();
    void applyDocumentTypography();
    void reapplyTypographyToChange();
    void scheduleRecovery();
    void persistDocument();
    void writeRecovery();
    void restoreRecovery();
    void clearRecovery();
    void setKnownFileContents(const QByteArray &contents, bool known);
    void watchCurrentFile();
    void loadOmarchyTheme();
    void watchOmarchyTheme();

    QUrl m_fileUrl;
    bool m_modified = false;
    QString m_status;
    int m_wordCount = 0;
    bool m_darkMode = true;
    qreal m_textScale = 1.0;
    int m_editorFontSize = 20;
    bool m_autosave = true;
    int m_autosaveDelayMs = 750;
    // An outside edit the writer has not answered yet. Autosave must not pick
    // a version for them while that dialog is standing.
    bool m_externalChangeUnanswered = false;
    bool m_loading = false;
    bool m_closeAfterSave = false;
    bool m_formattingTypography = false;
    int m_formattedBlockCount = 0;
    int m_lastChangePos = 0;
    int m_lastChangeAdded = 0;
    QTimer m_wordCountTimer;
    QTimer m_recoveryTimer;
    QFileSystemWatcher m_fileWatcher;
    QPointer<QTextDocument> m_document;
    QPointer<QWindow> m_parentWindow;
    QPointer<MarkdownHighlighter> m_highlighter;
    QString m_lastDocumentText;
    QByteArray m_lastKnownFileContents;
    QString m_lastKnownFileText;
    bool m_hasKnownFileContents = false;
    // Set where this document takes a name without having read what is on it,
    // and cleared the moment anything settles the question -- a read, a write,
    // or the writer answering the dialog. It is not the same question as
    // m_hasKnownFileContents, which asks whether we hold a copy to compare
    // against; a path we have never looked at is one nothing can watch.
    bool m_pathNeverRead = false;
    QString m_recoveryPath;
    std::unique_ptr<QLockFile> m_recoveryLock;

    QString m_themeBackground;
    QString m_themeForeground;
    QString m_themeAccent;
    QString m_themeSelection;
    QString m_themeLighterBackground;
    QString m_themeCodeBackground;
    QFileSystemWatcher m_themeWatcher;
};
