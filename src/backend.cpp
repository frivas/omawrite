#include "backend.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QPrintDialog>
#include <QPrinter>
#include <QQuickTextDocument>
#include <QFontDatabase>
#include <QScreen>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>

#include "markdownhighlighter.h"

constexpr qreal typoraLineHeightPercent = 140;
const QString lastSaveDirectorySetting = QStringLiteral("file/lastSaveDirectory");
const QString editorFontSizeSetting = QStringLiteral("editor/fontSize");
constexpr int defaultEditorFontSize = 20;
constexpr int minimumEditorFontSize = 10;
constexpr int maximumEditorFontSize = 48;
const QString editorFontFamilySetting = QStringLiteral("editor/fontFamily");
const QString caretStyleSetting = QStringLiteral("editor/caretStyle");
const QString caretBlinkSetting = QStringLiteral("editor/caretBlink");
const QString editorMeasureCharsSetting = QStringLiteral("editor/measureChars");
const QString printMarginMmSetting = QStringLiteral("print/marginMm");
// Shipped with the app, so these are the families that are always there.
// Quattro leads: four character widths, and the face iA Writer defaults to.
const QString bundledFontFamily = QStringLiteral("iA Writer Quattro S");
constexpr int defaultEditorMeasureChars = 65;
constexpr int minimumEditorMeasureChars = 20;
constexpr int maximumEditorMeasureChars = 200;
constexpr qreal defaultPrintMarginMm = 20.0;
constexpr qreal maximumPrintMarginMm = 60.0;
const QString wordTargetSetting = QStringLiteral("editor/wordTarget");
const QString autosaveSetting = QStringLiteral("editor/autosave");
const QString autosaveDelaySetting = QStringLiteral("editor/autosaveDelayMs");
constexpr int defaultAutosaveDelayMs = 750;
// Below a fifth of a second the debounce fires mid-word; above a minute it is
// no longer the safety net it is here to be.
constexpr int minimumAutosaveDelayMs = 200;
constexpr int maximumAutosaveDelayMs = 60000;

namespace {

// A shade of `from` carried part of the way towards `to`.
QColor blend(const QColor &from, const QColor &to, qreal amount) {
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

}

QString Backend::normalizedLinkUrl(const QString &clipboardText) {
    QString candidate = clipboardText.trimmed();
    static const QRegularExpression lineBreakRe(QStringLiteral("[\\r\\n]"));
    const int lineBreak = candidate.indexOf(lineBreakRe);
    if (lineBreak >= 0)
        candidate = candidate.left(lineBreak).trimmed();

    if (candidate.isEmpty())
        return {};

    if (candidate.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
        candidate.prepend(QStringLiteral("https://"));

    static const QRegularExpression schemeRe(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    if (!schemeRe.match(candidate).hasMatch())
        return {};

    const QUrl url(candidate);
    if (!url.isValid() || url.scheme().isEmpty())
        return {};

    const QString scheme = url.scheme().toLower();
    const bool webUrl = scheme == QStringLiteral("http")
        || scheme == QStringLiteral("https")
        || scheme == QStringLiteral("ftp");
    if (webUrl && url.host().isEmpty())
        return {};

    if (!webUrl && scheme != QStringLiteral("mailto"))
        return {};

    return url.toString();
}

Backend::Backend(QObject *parent) : QObject(parent) {
    m_editorFontSize = qBound(minimumEditorFontSize,
                              QSettings().value(editorFontSizeSetting,
                                                defaultEditorFontSize).toInt(),
                              maximumEditorFontSize);

    const QString stateDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(stateDirectory);
    // Claim an orphaned snapshot before taking an empty slot. This ensures a
    // crash in window 2 is still recovered even if window 1 exited normally.
    for (int pass = 0; pass < 2 && !m_recoveryLock; ++pass) {
        for (int slot = 0; slot < 100; ++slot) {
            const QString base = QDir(stateDirectory).filePath(
                QStringLiteral("recovery-%1").arg(slot));
            const bool snapshotExists = QFileInfo::exists(base + QStringLiteral(".json"));
            if ((pass == 0) != snapshotExists)
                continue;
            auto lock = std::make_unique<QLockFile>(base + QStringLiteral(".lock"));
            if (lock->tryLock()) {
                m_recoveryPath = base + QStringLiteral(".json");
                m_recoveryLock = std::move(lock);
                break;
            }
        }
    }
    m_wordCountTimer.setSingleShot(true);
    m_wordCountTimer.setInterval(120);
    connect(&m_wordCountTimer, &QTimer::timeout, this, &Backend::refreshWordCount);
    const QString requestedFamily =
        QSettings().value(editorFontFamilySetting).toString();
    m_editorFontFamily = requestedFamily.isEmpty()
        ? bundledFontFamily
        : resolveFontFamily(requestedFamily, availableFontFamilies());
    m_caretStyle = QSettings().value(caretStyleSetting,
                                     QStringLiteral("line")).toString()
                   == QStringLiteral("block")
        ? QStringLiteral("block")
        : QStringLiteral("line");
    m_caretBlink = QSettings().value(caretBlinkSetting, true).toBool();
    m_editorMeasureChars = qBound(minimumEditorMeasureChars,
                                  QSettings().value(editorMeasureCharsSetting,
                                                    defaultEditorMeasureChars).toInt(),
                                  maximumEditorMeasureChars);
    m_printMarginMm = qBound(qreal(0), QSettings().value(printMarginMmSetting,
                                                         defaultPrintMarginMm).toDouble(),
                             maximumPrintMarginMm);

    m_wordTarget = qMax(0, QSettings().value(wordTargetSetting, 0).toInt());
    m_autosave = QSettings().value(autosaveSetting, true).toBool();
    m_autosaveDelayMs = qBound(minimumAutosaveDelayMs,
                               QSettings().value(autosaveDelaySetting,
                                                 defaultAutosaveDelayMs).toInt(),
                               maximumAutosaveDelayMs);
    m_recoveryTimer.setSingleShot(true);
    m_recoveryTimer.setInterval(m_autosaveDelayMs);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &Backend::persistDocument);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, this,
            [this](const QString &path) {
                if (path != m_fileUrl.toLocalFile())
                    return;

                const bool deleted = !QFileInfo::exists(path);
                if (!deleted && m_hasKnownFileContents) {
                    QFile file(path);
                    if (file.open(QIODevice::ReadOnly)
                            && file.readAll() == m_lastKnownFileContents) {
                        // Atomic saves can replace the watched inode. Re-arm the
                        // watcher, but do not report our own save as an outside edit.
                        watchCurrentFile();
                        return;
                    }
                }

                // Whatever is on disk now, it is not what we last read, so the
                // baseline is unknown until the writer picks a version. A
                // deletion counts: the old text is not saved anywhere either.
                setKnownFileContents(QByteArray(), false);
                m_externalChangeUnanswered = true;
                emit externalChangeDetected(deleted, m_modified);
            });

    loadOmarchyTheme();
    watchOmarchyTheme();
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
}

Backend::~Backend() = default;

void Backend::setParentWindow(QWindow *window) {
    m_parentWindow = window;
}

QString Backend::fileName() const {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty())
        return QStringLiteral("Untitled.md");

    if (m_fileUrl.isLocalFile()) {
        const QFileInfo info(m_fileUrl.toLocalFile());
        if (!info.fileName().isEmpty())
            return info.fileName();
    }

    const QString name = m_fileUrl.fileName();
    return name.isEmpty() ? QStringLiteral("Untitled.md") : name;
}

void Backend::setDarkMode(bool darkMode) {
    if (m_darkMode == darkMode)
        return;

    m_darkMode = darkMode;
    loadOmarchyTheme();
    emit darkModeChanged();
}

void Backend::setTextScale(qreal textScale) {
    if (qFuzzyCompare(m_textScale, textScale))
        return;

    m_textScale = textScale;
    emit textScaleChanged();
}

void Backend::setEditorFontSize(int editorFontSize) {
    const int boundedSize = qBound(minimumEditorFontSize, editorFontSize,
                                   maximumEditorFontSize);
    if (m_editorFontSize == boundedSize)
        return;

    m_editorFontSize = boundedSize;
    QSettings().setValue(editorFontSizeSetting, m_editorFontSize);
    emit editorFontSizeChanged();
}

QStringList Backend::bundledFontFamilies() {
    return {QStringLiteral("iA Writer Quattro S"),
            QStringLiteral("iA Writer Duo S"),
            QStringLiteral("iA Writer Mono S")};
}

QStringList Backend::availableFontFamilies() {
    QStringList families = QFontDatabase::families();
    // The app's own faces lead the list. Otherwise they sit somewhere in the
    // middle of a few hundred system families and read as unavailable.
    for (const QString &bundled : bundledFontFamilies())
        families.removeAll(bundled);
    return bundledFontFamilies() + families;
}

QString Backend::resolveFontFamily(const QString &requested,
                                   const QStringList &availableFamilies) {
    if (!requested.isEmpty() && availableFamilies.contains(requested))
        return requested;

    return bundledFontFamily;
}

void Backend::setEditorFontFamily(const QString &family) {
    const QString resolved = resolveFontFamily(family, QFontDatabase::families());
    if (m_editorFontFamily == resolved)
        return;

    m_editorFontFamily = resolved;
    QSettings().setValue(editorFontFamilySetting, m_editorFontFamily);
    if (m_document) {
        QFont font = m_document->defaultFont();
        font.setFamily(m_editorFontFamily);
        m_document->setDefaultFont(font);
    }
    emit editorFontFamilyChanged();
}

void Backend::setCaretStyle(const QString &caretStyle) {
    const QString normalized = caretStyle == QStringLiteral("block")
        ? QStringLiteral("block")
        : QStringLiteral("line");
    if (m_caretStyle == normalized)
        return;

    m_caretStyle = normalized;
    QSettings().setValue(caretStyleSetting, m_caretStyle);
    emit caretStyleChanged();
}

void Backend::setCaretBlink(bool caretBlink) {
    if (m_caretBlink == caretBlink)
        return;

    m_caretBlink = caretBlink;
    QSettings().setValue(caretBlinkSetting, m_caretBlink);
    emit caretBlinkChanged();
}

void Backend::setEditorMeasureChars(int measureChars) {
    const int bounded = qBound(minimumEditorMeasureChars, measureChars,
                               maximumEditorMeasureChars);
    if (m_editorMeasureChars == bounded)
        return;

    m_editorMeasureChars = bounded;
    QSettings().setValue(editorMeasureCharsSetting, m_editorMeasureChars);
    emit editorMeasureCharsChanged();
}

void Backend::setPrintMarginMm(qreal marginMm) {
    const qreal bounded = qBound(qreal(0), marginMm, maximumPrintMarginMm);
    if (qFuzzyCompare(m_printMarginMm, bounded))
        return;

    m_printMarginMm = bounded;
    QSettings().setValue(printMarginMmSetting, m_printMarginMm);
    emit printMarginMmChanged();
}

int Backend::draftTargetFor(int wordTarget) {
    if (wordTarget <= 0)
        return 0;

    // A first draft wants a quarter more than the finished piece, so there is
    // something to throw away rather than something to pad.
    return qRound(wordTarget * 1.25);
}

void Backend::setWordTarget(int wordTarget) {
    const int bounded = qMax(0, wordTarget);
    if (m_wordTarget == bounded)
        return;

    m_wordTarget = bounded;
    QSettings().setValue(wordTargetSetting, m_wordTarget);
    emit wordTargetChanged();
}

void Backend::setAutosave(bool autosave) {
    if (m_autosave == autosave)
        return;

    m_autosave = autosave;
    QSettings().setValue(autosaveSetting, m_autosave);
    emit autosaveChanged();
}

void Backend::setAutosaveDelayMs(int autosaveDelayMs) {
    const int bounded = qBound(minimumAutosaveDelayMs, autosaveDelayMs,
                               maximumAutosaveDelayMs);
    if (m_autosaveDelayMs == bounded)
        return;

    m_autosaveDelayMs = bounded;
    m_recoveryTimer.setInterval(m_autosaveDelayMs);
    QSettings().setValue(autosaveDelaySetting, m_autosaveDelayMs);
    emit autosaveDelayMsChanged();
}

void Backend::resetEditorFontSize() {
    setEditorFontSize(defaultEditorFontSize);
}

void Backend::attachDocument(QObject *textDocument) {
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    if (!quickDocument || !quickDocument->textDocument()) {
        setStatus(QStringLiteral("Could not attach the Markdown renderer."));
        return;
    }

    if (m_highlighter)
        delete m_highlighter.data();

    m_document = quickDocument->textDocument();
    m_lastDocumentText = m_document->toPlainText();
    m_highlighter = new MarkdownHighlighter(m_document);
    m_highlighter->setDarkMode(m_darkMode);
    m_highlighter->setColors(m_themeBackground, m_themeForeground, m_themeAccent,
                             m_themeCodeBackground);

    connect(m_document, &QTextDocument::contentsChange, this,
            [this](int position, int, int charsAdded) {
                if (m_formattingTypography || m_loading)
                    return;
                m_lastChangePos = position;
                m_lastChangeAdded = charsAdded;
            });

    applyDocumentTypography();
    restoreRecovery();
}

void Backend::openDialog() {
    emit openDialogRequested();
}

void Backend::open(const QUrl &url) {
    openPath(url, true);
}

void Backend::openPath(const QUrl &url, bool mayStartNewFile) {
    if (!url.isLocalFile()) {
        setStatus(QStringLiteral("Only local files can be opened."));
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QFile file(url.toLocalFile());
    // A path that is not there yet is a file the writer means to start, so
    // take the name for a blank document. The first save then lands where
    // they said it should, instead of asking them again.
    if (mayStartNewFile && !file.exists()) {
        // Only where it could be written: a name under a directory that is not
        // there leaves the first save with nowhere to land and no dialog.
        const QFileInfo parentDirectory(QFileInfo(url.toLocalFile()).absolutePath());
        if (!parentDirectory.isDir() || !parentDirectory.isWritable()) {
            setStatus(QStringLiteral("Could not open %1.").arg(targetName));
            return;
        }

        loadDocumentText(QString());
        clearRecovery();
        m_lastKnownFileContents.clear();
        m_hasKnownFileContents = false;
        m_pathNeverRead = true;
        m_externalChangeUnanswered = false;
        setFileUrl(url);
        setModified(false);
        setStatus(QStringLiteral("New file %1").arg(fileName()));
        return;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // A reload with nothing left to read leaves this document holding a
        // name and no file, and the watcher let the path go when it went.
        // That is the state a new file starts in, so say so: if the file
        // comes back, the next save asks rather than replacing it unseen.
        if (!mayStartNewFile && !file.exists())
            m_pathNeverRead = true;

        setStatus(QStringLiteral("Could not open %1.").arg(targetName));
        return;
    }

    const QByteArray contents = file.readAll();
    loadDocumentText(QString::fromUtf8(contents));
    clearRecovery();
    setKnownFileContents(contents, true);
    m_pathNeverRead = false;
    m_externalChangeUnanswered = false;
    setFileUrl(url);
    watchCurrentFile();
    setModified(false);
    setStatus(QStringLiteral("Opened %1").arg(fileName()));
}

void Backend::save() {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty()) {
        saveAsDialog();
        return;
    }

    // Nothing can watch a file that is not there, so a name taken for a file
    // that has yet to be written is unguarded until this save: a `git pull` or
    // a sync client can put something on that path in the meantime and
    // QSaveFile::commit() would replace it without a word. Ask once, and only
    // once -- the flag is cleared by every answer the dialog can give, so a
    // file that turns out to be unreadable cannot leave the writer trapped in
    // a question they have already answered.
    if (m_pathNeverRead && m_fileUrl.isLocalFile()
            && QFileInfo::exists(m_fileUrl.toLocalFile())) {
        m_closeAfterSave = false;
        emit externalFileAppeared(m_modified);
        return;
    }

    saveTo(m_fileUrl);
}

void Backend::saveForClose() {
    if (!m_modified) {
        emit closeAfterSave();
        return;
    }

    m_closeAfterSave = true;
    save();
}

void Backend::saveAsDialog() {
    emit saveDialogRequested(suggestedSaveUrl());
}

void Backend::saveAs(const QUrl &url) {
    saveTo(url);
}

void Backend::fileDialogCanceled() {
    m_closeAfterSave = false;
}

void Backend::discardRecovery() {
    clearRecovery();
}

void Backend::reloadFromDisk() {
    // Reload is asked for a file we already have, so it must not go down the
    // path that takes an absent name for a new document: if the file goes away
    // between the "File changed" dialog opening and the click, blanking the
    // editor and clearing recovery would throw away the only copy left. Say
    // it could not be opened and leave the text where it is.
    if (m_fileUrl.isLocalFile())
        openPath(m_fileUrl, false);
}

void Backend::keepExternalVersion() {
    m_externalChangeUnanswered = false;
    QFile file(m_fileUrl.toLocalFile());
    if (file.open(QIODevice::ReadOnly)) {
        setKnownFileContents(file.readAll(), true);
    } else {
        setKnownFileContents(QByteArray(), false);
    }
    // Answered, whether or not the file could be read. Failing to read it is
    // not a reason to ask again: the writer said to keep their version, and
    // the next save must be allowed to try, so the filesystem gets to give
    // the answer instead of the dialog asking the same question forever.
    m_pathNeverRead = false;
    setModified(true);
    scheduleRecovery();
    watchCurrentFile();
    setStatus(QStringLiteral("Kept your version"));
}

QFont Backend::printFont(const QFont &editorFont, qreal screenDpi) {
    QFont font = editorFont;
    if (font.pixelSize() <= 0)
        return font;

    // A pixel size is a screen measurement. Handing it to a HighResolution
    // QPrinter makes the printer read it as 1200-dpi dots, so an 18px font
    // prints 18/1200 of an inch tall. Points are resolution-independent.
    const qreal dpi = screenDpi > 0.0 ? screenDpi : 96.0;
    font.setPointSizeF(font.pixelSize() * 72.0 / dpi);
    return font;
}

QString Backend::printJobName(const QString &documentFileName) {
    const QString trimmed = documentFileName.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("Untitled");

    const QFileInfo info(trimmed);
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown"))
        return info.completeBaseName();

    return trimmed;
}

void Backend::renderPreview(QObject *textDocument) {
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    if (!quickDocument || !quickDocument->textDocument())
        return;

    QTextDocument *preview = quickDocument->textDocument();
    preview->setDefaultFont(m_document ? m_document->defaultFont() : preview->defaultFont());
    preview->setMarkdown(currentDocumentText());

    // Qt's Markdown reader packs every block flush against the next, so the
    // blank lines that separate paragraphs in the source disappear from the
    // render. Put that breathing room back as real block spacing, sized from
    // the editor font so it tracks the desktop text scale.
    const qreal gap = preview->defaultFont().pixelSize() > 0
                          ? preview->defaultFont().pixelSize() * 0.75
                          : 15.0;

    QTextCursor cursor(preview);
    cursor.beginEditBlock();
    for (QTextBlock block = preview->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        const bool heading = format.headingLevel() > 0;
        const bool listItem = block.textList() != nullptr;
        const bool quote = format.intProperty(QTextFormat::BlockQuoteLevel) > 0;

        // Headings lead their section, list items stay tight together, and
        // ordinary paragraphs get a full blank line's worth beneath them.
        // Quotes need the leading gap too or they read as another list row.
        format.setTopMargin(heading ? gap * 1.5 : (quote ? gap : 0));
        format.setBottomMargin(listItem ? gap * 0.2 : gap);
        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);
    }
    cursor.endEditBlock();
}

namespace {
// A paragraph is a run of non-blank lines. Returns the [start, end) offsets of
// the paragraph containing `cursor`, clamped to the text.
struct ParagraphRange { int start; int end; };

ParagraphRange paragraphAround(const QStringList &lines, int lineIndex) {
    int start = lineIndex;
    while (start > 0 && !lines.at(start - 1).trimmed().isEmpty())
        --start;
    int end = lineIndex;
    while (end + 1 < lines.size() && !lines.at(end + 1).trimmed().isEmpty())
        ++end;
    return {start, end};
}

int lineIndexForOffset(const QStringList &lines, int cursor) {
    int consumed = 0;
    for (int i = 0; i < lines.size(); ++i) {
        consumed += lines.at(i).size() + 1;  // the newline
        if (cursor < consumed)
            return i;
    }
    return qMax(0, lines.size() - 1);
}

int offsetForLineIndex(const QStringList &lines, int lineIndex) {
    int offset = 0;
    for (int i = 0; i < lineIndex && i < lines.size(); ++i)
        offset += lines.at(i).size() + 1;
    return offset;
}

QVariantMap result(const QString &text, int cursor) {
    return QVariantMap{{QStringLiteral("text"), text},
                       {QStringLiteral("cursor"), qBound(0, cursor, text.size())}};
}

// Abbreviations that end in a full stop without ending a sentence. Deliberately
// short: a longer list is a dictionary, and the cost of a wrong split here is
// one keystroke to undo, not lost work.
bool endsWithAbbreviation(const QString &sentence) {
    static const QStringList abbreviations{
        QStringLiteral("mr."),  QStringLiteral("mrs."), QStringLiteral("ms."),
        QStringLiteral("dr."),  QStringLiteral("prof."), QStringLiteral("st."),
        QStringLiteral("e.g."), QStringLiteral("i.e."), QStringLiteral("etc."),
        QStringLiteral("vs."),  QStringLiteral("cf."),  QStringLiteral("al.")};

    const QString tail = sentence.trimmed().toLower();
    for (const QString &abbreviation : abbreviations) {
        if (tail.endsWith(abbreviation))
            return true;
    }

    // A single initial, as in "J. B. Peterson".
    if (tail.size() >= 2 && tail.endsWith(QLatin1Char('.'))
            && tail.at(tail.size() - 2).isLetter()
            && (tail.size() == 2 || !tail.at(tail.size() - 3).isLetter())) {
        return true;
    }

    return false;
}
}

QStringList Backend::splitSentences(const QString &paragraph) {
    QStringList sentences;
    const QString text = paragraph.simplified();
    if (text.isEmpty())
        return sentences;

    int start = 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar character = text.at(i);
        if (character != QLatin1Char('.') && character != QLatin1Char('!')
                && character != QLatin1Char('?')) {
            continue;
        }

        // Run on through ?!. and any closing quote or bracket.
        int end = i;
        while (end + 1 < text.size()
               && QStringLiteral(".!?\"')]").contains(text.at(end + 1))) {
            ++end;
        }

        if (end + 1 >= text.size()) {
            sentences << text.mid(start).trimmed();
            start = text.size();
            break;
        }

        if (text.at(end + 1) != QLatin1Char(' '))
            continue;

        const QString candidate = text.mid(start, end - start + 1);
        if (endsWithAbbreviation(candidate))
            continue;

        // A decimal such as "3.5" is not a boundary.
        if (character == QLatin1Char('.') && i > 0 && text.at(i - 1).isDigit()
                && end + 2 < text.size() && text.at(end + 2).isDigit()) {
            continue;
        }

        sentences << candidate.trimmed();
        start = end + 2;
        i = end + 1;
    }

    if (start < text.size()) {
        const QString rest = text.mid(start).trimmed();
        if (!rest.isEmpty())
            sentences << rest;
    }

    return sentences;
}

QVariantMap Backend::moveParagraph(const QString &text, int cursor, int delta) {
    if (delta == 0)
        return result(text, cursor);

    QStringList lines = text.split(QLatin1Char('\n'));
    const int lineIndex = lineIndexForOffset(lines, cursor);
    const ParagraphRange here = paragraphAround(lines, lineIndex);
    if (lines.at(here.start).trimmed().isEmpty())
        return result(text, cursor);  // the cursor is between paragraphs

    // Find the neighbouring paragraph in the direction asked for.
    int probe = delta < 0 ? here.start - 1 : here.end + 1;
    while (probe >= 0 && probe < lines.size() && lines.at(probe).trimmed().isEmpty())
        probe += delta < 0 ? -1 : 1;
    if (probe < 0 || probe >= lines.size())
        return result(text, cursor);  // nothing to trade places with

    const ParagraphRange there = paragraphAround(lines, probe);
    const int cursorWithin = cursor - offsetForLineIndex(lines, here.start);

    const QStringList moving = lines.mid(here.start, here.end - here.start + 1);
    const QStringList other = lines.mid(there.start, there.end - there.start + 1);

    QStringList rebuilt;
    if (delta < 0) {
        rebuilt = lines.mid(0, there.start) + moving + QStringList{QString()} + other
                + lines.mid(here.end + 1);
    } else {
        rebuilt = lines.mid(0, here.start) + other + QStringList{QString()} + moving
                + lines.mid(there.end + 1);
    }

    const QString rebuiltText = rebuilt.join(QLatin1Char('\n'));
    const int newStartLine = delta < 0 ? there.start
                                       : there.start - (here.end - here.start + 1);
    const int newCursor = offsetForLineIndex(rebuilt, qMax(0, newStartLine)) + cursorWithin;
    return result(rebuiltText, newCursor);
}

QVariantMap Backend::explodeSentences(const QString &text, int cursor) {
    QStringList lines = text.split(QLatin1Char('\n'));
    const int lineIndex = lineIndexForOffset(lines, cursor);
    const ParagraphRange range = paragraphAround(lines, lineIndex);
    if (lines.at(range.start).trimmed().isEmpty())
        return result(text, cursor);

    const QString paragraph =
        lines.mid(range.start, range.end - range.start + 1).join(QLatin1Char(' '));
    const QStringList sentences = splitSentences(paragraph);
    if (sentences.size() < 2)
        return result(text, cursor);

    const QStringList rebuilt =
        lines.mid(0, range.start) + sentences + lines.mid(range.end + 1);
    const QString rebuiltText = rebuilt.join(QLatin1Char('\n'));
    return result(rebuiltText, offsetForLineIndex(rebuilt, range.start));
}

QVariantMap Backend::collapseSentences(const QString &text, int cursor) {
    QStringList lines = text.split(QLatin1Char('\n'));
    const int lineIndex = lineIndexForOffset(lines, cursor);
    const ParagraphRange range = paragraphAround(lines, lineIndex);
    if (lines.at(range.start).trimmed().isEmpty())
        return result(text, cursor);

    QStringList parts;
    for (int i = range.start; i <= range.end; ++i) {
        const QString trimmed = lines.at(i).trimmed();
        if (!trimmed.isEmpty())
            parts << trimmed;
    }

    const QStringList rebuilt = lines.mid(0, range.start)
        + QStringList{parts.join(QLatin1Char(' '))} + lines.mid(range.end + 1);
    const QString rebuiltText = rebuilt.join(QLatin1Char('\n'));
    return result(rebuiltText, offsetForLineIndex(rebuilt, range.start));
}

QVariantList Backend::outlineFor(const QString &text) {
    QVariantList outline;
    static const QRegularExpression headingRe(
        QStringLiteral("^(#{1,6})\\s+(.*\\S)\\s*$"));

    const QStringList lines = text.split(QLatin1Char('\n'));
    int offset = 0;
    bool inFence = false;
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("```"))
                || trimmed.startsWith(QStringLiteral("~~~"))) {
            inFence = !inFence;
        } else if (!inFence) {
            const QRegularExpressionMatch match = headingRe.match(line);
            if (match.hasMatch()) {
                outline.append(QVariantMap{
                    {QStringLiteral("level"), match.captured(1).size()},
                    {QStringLiteral("title"), match.captured(2)},
                    {QStringLiteral("position"), offset}});
            }
        }
        offset += line.size() + 1;
    }

    return outline;
}

void Backend::printDocument() {
    if (!m_document) {
        setStatus(QStringLiteral("There is no document to print."));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    // Drives the default filename macOS puts in Save as PDF.
    printer.setDocName(printJobName(fileName()));
    QPrintDialog dialog(&printer);
    dialog.setWindowTitle(QStringLiteral("Print %1").arg(fileName()));
    dialog.winId();
    if (dialog.windowHandle() && m_parentWindow)
        dialog.windowHandle()->setTransientParent(m_parentWindow);

    if (dialog.exec() == QDialog::Accepted) {
        QPageLayout layout = printer.pageLayout();
        layout.setUnits(QPageLayout::Millimeter);
        layout.setMargins(QMarginsF(m_printMarginMm, m_printMarginMm,
                                    m_printMarginMm, m_printMarginMm));
        printer.setPageLayout(layout);

        QTextDocument rendered;
        const QScreen *screen = QGuiApplication::primaryScreen();
        QFont printed = printFont(m_document->defaultFont(),
                                  screen ? screen->logicalDotsPerInchY() : 0.0);
        // The page follows the editor, so what is printed reads as what was
        // written rather than as a different document.
        printed.setFamily(m_editorFontFamily);
        rendered.setDefaultFont(printed);
        rendered.setMarkdown(currentDocumentText());
        rendered.print(&printer);
    }
}

QString Backend::enclosingBundlePath(const QString &executableDirPath) {
    // Walked as text rather than with QDir::cdUp(), which needs the directory
    // to exist and so could not be tested without building a bundle.
    const QStringList parts = QDir::cleanPath(executableDirPath).split(QLatin1Char('/'));
    if (parts.size() < 3)
        return {};

    if (parts.at(parts.size() - 1) != QStringLiteral("MacOS")
        || parts.at(parts.size() - 2) != QStringLiteral("Contents")
        || !parts.at(parts.size() - 3).endsWith(QStringLiteral(".app"))) {
        return {};
    }

    return QStringList(parts.mid(0, parts.size() - 2)).join(QLatin1Char('/'));
}

bool Backend::launchNewInstance(const QString &filePath) {
    QStringList arguments;

#ifdef Q_OS_MACOS
    // Running the executable inside the bundle directly gives the new instance
    // its own Dock tile and leaves it unknown to Launch Services. `open -n`
    // asks macOS for a second instance of the application itself.
    const QString bundle = enclosingBundlePath(QCoreApplication::applicationDirPath());
    if (!bundle.isEmpty()) {
        arguments << QStringLiteral("-n") << QStringLiteral("-a") << bundle;
        if (!filePath.isEmpty())
            arguments << QStringLiteral("--args") << filePath;

        return QProcess::startDetached(QStringLiteral("open"), arguments);
    }
#endif

    if (!filePath.isEmpty())
        arguments << filePath;

    return QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments);
}

void Backend::newWindow() {
    if (!launchNewInstance())
        setStatus(QStringLiteral("Could not open a new window."));
}

QString Backend::clipboardUrl() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    if (!mimeData)
        return {};

    if (mimeData->hasUrls()) {
        const QList<QUrl> urls = mimeData->urls();
        for (const QUrl &url : urls) {
            const QString normalized = normalizedLinkUrl(url.toString());
            if (!normalized.isEmpty())
                return normalized;
        }
    }

    if (!mimeData->hasText())
        return {};

    return normalizedLinkUrl(mimeData->text());
}

QString Backend::clipboardText() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    return mimeData && mimeData->hasText() ? mimeData->text() : QString();
}

bool Backend::editorTextChanged() {
    if (m_loading || m_formattingTypography)
        return false;

    const QString text = currentDocumentText();
    if (text == m_lastDocumentText)
        return false;
    m_lastDocumentText = text;

    if (m_document) {
        const int blockCount = m_document->blockCount();
        if (blockCount > m_formattedBlockCount)
            reapplyTypographyToChange();
        m_formattedBlockCount = blockCount;
    }

    scheduleWordCount();

    const QString &baseline = m_lastKnownFileText;
    // An empty baseline means a pristine untitled document, but only when there
    // is no file behind it. Once there is one, an unknown baseline is unknown
    // rather than empty, and emptying the editor is a change like any other.
    const bool baselineKnown = m_hasKnownFileContents
        || !m_fileUrl.isValid() || m_fileUrl.isEmpty();

    if (baselineKnown && text == baseline) {
        setModified(false);
        clearRecovery();
        if (m_hasKnownFileContents)
            setStatus(QStringLiteral("Saved %1").arg(fileName()));
        else
            setStatus(QString());
    } else {
        setModified(true);
        setStatus(QStringLiteral("Unsaved"));
        scheduleRecovery();
    }

    return true;
}

QVariantList Backend::hiddenRangesAt(int position) const {
    QVariantList ranges;
    if (!m_document)
        return ranges;

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid())
        return ranges;

    // The highlighter leaves fenced code alone, so it hides nothing there.
    if (block.userState() == MarkdownHighlighter::InsideFence)
        return ranges;

    const int lineStart = block.position();
    QList<QPair<int, int>> spans;
    const QList<MarkdownHighlighter::InlineMarkup> markup =
        MarkdownHighlighter::inlineMarkup(block.text());
    for (const MarkdownHighlighter::InlineMarkup &item : markup) {
        for (const MarkdownHighlighter::Span &marker : item.markers) {
            spans.append({lineStart + marker.start,
                          lineStart + marker.start + marker.length});
        }
    }
    std::sort(spans.begin(), spans.end());

    for (const auto &span : spans) {
        ranges.append(QVariantMap{{QStringLiteral("start"), span.first},
                                  {QStringLiteral("end"), span.second}});
    }
    return ranges;
}

void Backend::setSearchHighlight(const QString &query, int currentMatchStart) {
    if (m_highlighter)
        m_highlighter->setSearch(query, currentMatchStart);
}

void Backend::openExternalUrl(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https")
            || scheme == QStringLiteral("mailto"))
        QDesktopServices::openUrl(url);
}

QVariantMap Backend::windowGeometry() const {
    QSettings settings;
    return {{QStringLiteral("x"), settings.value(QStringLiteral("window/x"), -1)},
            {QStringLiteral("y"), settings.value(QStringLiteral("window/y"), -1)},
            {QStringLiteral("width"), settings.value(QStringLiteral("window/width"), 1280)},
            {QStringLiteral("height"), settings.value(QStringLiteral("window/height"), 820)},
            {QStringLiteral("maximized"), settings.value(QStringLiteral("window/maximized"), false)}};
}

void Backend::saveWindowGeometry(int x, int y, int width, int height, bool maximized) {
    QSettings settings;
    if (!maximized) {
        settings.setValue(QStringLiteral("window/x"), x);
        settings.setValue(QStringLiteral("window/y"), y);
        settings.setValue(QStringLiteral("window/width"), width);
        settings.setValue(QStringLiteral("window/height"), height);
    }
    settings.setValue(QStringLiteral("window/maximized"), maximized);
}

void Backend::loadDocumentText(const QString &text) {
    if (!m_document) {
        setStatus(QStringLiteral("Could not attach the Markdown renderer."));
        return;
    }

    m_loading = true;
    m_document->setPlainText(text);
    m_lastDocumentText = text;
    m_loading = false;

    applyDocumentTypography();
    m_wordCountTimer.stop();
    setWordCount(countWords(text));
}

void Backend::setFileUrl(const QUrl &url) {
    if (m_fileUrl == url)
        return;

    m_fileUrl = url;
    emit fileUrlChanged();
    watchCurrentFile();
}

void Backend::setModified(bool modified) {
    if (m_modified == modified)
        return;

    m_modified = modified;
    emit modifiedChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;

    m_status = status;
    emit statusChanged();
}

void Backend::saveTo(const QUrl &url) {
    if (!url.isLocalFile()) {
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Only local files can be saved."));
        emit saveFailed();
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QSaveFile file(url.toLocalFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Could not save %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    const QByteArray contents = currentDocumentText().toUtf8();
    file.write(contents);

    // QSaveFile commits by replacing the target. Stop watching the old inode
    // before that replacement so our own write is not classified as external.
    const QStringList watched = m_fileWatcher.files();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);

    // commit() flushes, fsyncs, and atomically renames the temp file into place,
    // returning false (and leaving the original untouched) on any write error.
    if (!file.commit()) {
        watchCurrentFile();
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Could not write %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    const bool shouldClose = m_closeAfterSave;
    m_closeAfterSave = false;
    setKnownFileContents(contents, true);
    m_pathNeverRead = false;
    setFileUrl(url);
    watchCurrentFile();
    QSettings().setValue(lastSaveDirectorySetting,
                         QFileInfo(url.toLocalFile()).absolutePath());
    setModified(false);
    setStatus(QStringLiteral("Saved %1").arg(fileName()));
    clearRecovery();
    emit saveSucceeded();

    if (shouldClose)
        emit closeAfterSave();
}

void Backend::scheduleRecovery() {
    m_recoveryTimer.start();
}

bool Backend::canAutosaveToFile() const {
    if (!m_autosave || !m_fileUrl.isLocalFile())
        return false;

    // An outside edit nobody has answered yet. The writer picks the version
    // that survives; autosave does not get to pick it for them.
    if (m_externalChangeUnanswered)
        return false;

    // A name taken for a file that was never read is unguarded: nothing can
    // watch a path with no file, so something can arrive on it between the
    // naming and the write. The first save is the one that asks about that,
    // so leave it to the writer -- until they have saved once, this document
    // keeps getting the snapshot instead.
    if (m_pathNeverRead)
        return false;

    return true;
}

// The debounce that used to only ever write the crash snapshot. A document
// with a name it is safe to write goes to its own file; everything else --
// untitled drafts, and anything the guardrails hold back -- keeps getting the
// snapshot, so no edit is ever only in memory.
void Backend::persistDocument() {
    if (!m_modified)
        return;

    if (canAutosaveToFile()) {
        saveTo(m_fileUrl);
        return;
    }

    writeRecovery();
}

QString Backend::recoveryPath() const {
    return m_recoveryPath;
}

void Backend::setKnownFileContents(const QByteArray &contents, bool known) {
    m_lastKnownFileContents = contents;
    m_hasKnownFileContents = known;
    m_lastKnownFileText = known
        ? QString::fromUtf8(contents).replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
        : QString();
}

void Backend::writeRecovery() {
    if (!m_modified)
        return;
    const QString path = recoveryPath();
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    const QJsonObject recovery{{QStringLiteral("fileUrl"), m_fileUrl.toString()},
                               {QStringLiteral("pathNeverRead"), m_pathNeverRead},
                               {QStringLiteral("text"), currentDocumentText()}};
    file.write(QJsonDocument(recovery).toJson(QJsonDocument::Compact));
    file.commit();
}

void Backend::restoreRecovery() {
    QFile file(recoveryPath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    if (!json.isObject() || !json.object().contains(QStringLiteral("text")))
        return;
    const QJsonObject recovery = json.object();
    loadDocumentText(recovery.value(QStringLiteral("text")).toString());
    const QUrl recoveredUrl(recovery.value(QStringLiteral("fileUrl")).toString());
    QFile diskFile(recoveredUrl.toLocalFile());
    if (recoveredUrl.isLocalFile() && diskFile.open(QIODevice::ReadOnly)) {
        setKnownFileContents(diskFile.readAll(), true);
        // Reading it now says what is on the path, not that this document ever
        // looked: the file can have arrived while Omawrite was gone. Only the
        // snapshot knows, so a snapshot without the key predates the flag and
        // names a path something was written to.
        m_pathNeverRead = recovery.value(QStringLiteral("pathNeverRead")).toBool();
    } else {
        setKnownFileContents(QByteArray(), false);
        // A snapshot can name a file that was never written -- the crash came
        // first. That is the same unverified path a new file starts on.
        m_pathNeverRead = true;
    }
    setFileUrl(recoveredUrl);
    setModified(true);
    setStatus(QStringLiteral("Recovered unsaved changes"));
}

void Backend::clearRecovery() {
    m_recoveryTimer.stop();
    QFile::remove(recoveryPath());
}

void Backend::watchCurrentFile() {
    const QStringList watched = m_fileWatcher.files();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);
    if (m_fileUrl.isLocalFile() && QFileInfo::exists(m_fileUrl.toLocalFile()))
        m_fileWatcher.addPath(m_fileUrl.toLocalFile());
}

// A quoted TOML value ends at its closing quote; whatever trails it is an inline comment.
QString Backend::tomlValue(const QString &text) {
    const QString value = text.trimmed();
    if (value.isEmpty())
        return value;

    const QChar quote = value.front();
    if (quote != QLatin1Char('"') && quote != QLatin1Char('\''))
        return value;

    const int close = value.indexOf(quote, 1);
    return close < 0 ? value : value.mid(1, close - 1);
}

void Backend::loadOmarchyTheme() {
    const QString colorsPath = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");
    QString themeMode;
    QString background;
    QString foreground;
    QString accent;
    QString selection;
    // Left empty when the theme sets no lighter background; the highlighter
    // mixes its own shade of the page in that case.
    m_themeLighterBackground.clear();
    QFile file(colorsPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;

            const int equals = line.indexOf(QLatin1Char('='));
            if (equals < 0)
                continue;

            const QString key = line.left(equals).trimmed();
            const QString value = tomlValue(line.mid(equals + 1));

            if (key == QStringLiteral("mode"))
                themeMode = value;
            else if (key == QStringLiteral("background"))
                background = value;
            else if (key == QStringLiteral("foreground"))
                foreground = value;
            else if (key == QStringLiteral("accent"))
                accent = value;
            else if (key == QStringLiteral("selection"))
                selection = value;
            else if (key == QStringLiteral("lighter_background"))
                m_themeLighterBackground = value;
        }
    }

    bool themeIsDark = m_darkMode;
    if (themeMode == QStringLiteral("dark")) {
        themeIsDark = true;
    } else if (themeMode == QStringLiteral("light")) {
        themeIsDark = false;
    } else {
        const QColor parsedBackground(background);
        if (parsedBackground.isValid()) {
            const double luminance = 0.299 * parsedBackground.redF()
                + 0.587 * parsedBackground.greenF() + 0.114 * parsedBackground.blueF();
            themeIsDark = luminance < 0.5;
        }
    }
    const bool darkModeFlipped = themeIsDark != m_darkMode;
    m_darkMode = themeIsDark;

    // The defaults follow the mode the theme resolved to, so a colour we cannot
    // read costs that one colour rather than the contrast of the whole editor.
    m_themeBackground = m_darkMode ? QStringLiteral("#101010") : QStringLiteral("#ffffff");
    m_themeForeground = m_darkMode ? QStringLiteral("#eeeeee") : QStringLiteral("#222324");
    m_themeAccent = m_darkMode ? QStringLiteral("#5584aa") : QStringLiteral("#2077b2");
    m_themeSelection = m_darkMode ? QStringLiteral("#186a9a") : QStringLiteral("#2077b2");

    // An unparseable colour reaches QML as black, so keep the default instead.
    const auto applyColor = [](QString &target, const QString &value) {
        if (QColor(value).isValid())
            target = value;
    };
    applyColor(m_themeBackground, background);
    applyColor(m_themeForeground, foreground);
    applyColor(m_themeAccent, accent);
    applyColor(m_themeSelection, selection);

    // Omarchy themes name a lighter background for panels like the one code
    // sits on, so use the shade the theme chose. Not every theme sets the key,
    // and a few set it to the page background, which would leave code with no
    // panel at all: mix a shade of the page towards the text for those. Mixing,
    // unlike lightening, still moves on a pure black background.
    const QColor page(m_themeBackground);
    const QColor lighter(m_themeLighterBackground);
    m_themeCodeBackground = (lighter.isValid() && lighter != page
                                 ? lighter
                                 : blend(page, QColor(m_themeForeground), 0.06)).name();

    if (m_highlighter) {
        m_highlighter->setDarkMode(m_darkMode);
        m_highlighter->setColors(m_themeBackground, m_themeForeground, m_themeAccent,
                                 m_themeCodeBackground);
    }

    if (darkModeFlipped)
        emit darkModeChanged();
    emit themeColorsChanged();
}

void Backend::watchOmarchyTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const QString currentDir = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current");
    const QString themeDir = currentDir + QStringLiteral("/theme");
    const QString colorsPath = themeDir + QStringLiteral("/colors.toml");

    if (QDir(currentDir).exists())
        m_themeWatcher.addPath(currentDir);
    if (QDir(themeDir).exists())
        m_themeWatcher.addPath(themeDir);
    if (QFile::exists(colorsPath))
        m_themeWatcher.addPath(colorsPath);
}

QUrl Backend::suggestedSaveUrl() const {
    if (m_fileUrl.isLocalFile())
        return m_fileUrl;

    const QString savedDirectory = QSettings().value(lastSaveDirectorySetting).toString();
    const QDir directory = savedDirectory.isEmpty() || !QDir(savedDirectory).exists()
        ? QDir::home()
        : QDir(savedDirectory);
    return QUrl::fromLocalFile(
        directory.filePath(suggestedFileName(currentDocumentText())));
}

QString Backend::currentDocumentText() const {
    return m_document ? m_document->toPlainText() : QString();
}

int Backend::countWords(const QString &text) {
    static const QRegularExpression wordRe(
        QStringLiteral("[\\p{L}\\p{N}]+(?:['-][\\p{L}\\p{N}]+)*"));
    int count = 0;
    QRegularExpressionMatchIterator it = wordRe.globalMatch(text);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

QString Backend::suggestedFileName(const QString &text) {
    QString name = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\x00-\\x1f\\x7f]")),
                 QStringLiteral("-"));
    name = name.left(120).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("Untitled");
    if (!name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        name += QStringLiteral(".md");
    return name;
}

void Backend::setWordCount(int words) {
    if (m_wordCount == words)
        return;

    m_wordCount = words;
    emit wordCountChanged();
}

void Backend::refreshWordCount() {
    setWordCount(countWords(currentDocumentText()));
}

void Backend::scheduleWordCount() {
    m_wordCountTimer.start();
}

void Backend::applyDocumentTypography() {
    if (!m_document)
        return;

    QTextBlockFormat blockFormat;
    blockFormat.setLineHeight(typoraLineHeightPercent, QTextBlockFormat::ProportionalHeight);

    // A full pass is only used for freshly loaded/attached documents, so it is
    // safe to drop undo history here (re-enabling clears the stack anyway).
    const bool undoEnabled = m_document->isUndoRedoEnabled();
    m_document->setUndoRedoEnabled(false);

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(blockFormat);
    m_formattingTypography = false;

    m_document->setUndoRedoEnabled(undoEnabled);

    m_formattedBlockCount = m_document->blockCount();
}

void Backend::reapplyTypographyToChange() {
    if (!m_document)
        return;

    QTextBlockFormat blockFormat;
    blockFormat.setLineHeight(typoraLineHeightPercent, QTextBlockFormat::ProportionalHeight);

    // Format only the block(s) touched by the last edit instead of the whole
    // document, and fold the change into the preceding edit command so a single
    // undo reverts both the text and its formatting.
    const int maxPos = m_document->characterCount() - 1;
    const int start = qBound(0, m_lastChangePos, maxPos);
    const int end = qBound(start, m_lastChangePos + m_lastChangeAdded, maxPos);

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    cursor.joinPreviousEditBlock();
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.mergeBlockFormat(blockFormat);
    cursor.endEditBlock();
    m_formattingTypography = false;
}
