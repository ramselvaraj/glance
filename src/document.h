#pragma once

#include <QObject>
#include <QString>
#include <QSizeF>
#include <QPointF>
#include <QImage>
#include <QMutex>
#include <QVector>
#include <QVariantList>
#include <QVariantMap>
#include <QHash>
#include <QThreadPool>
#include <atomic>

#include <mupdf/fitz.h>

class Document : public QObject {
    Q_OBJECT
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pageCountChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY pageCountChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY pageCountChanged)
    Q_PROPERTY(bool ok READ ok NOTIFY pageCountChanged)

public:
    // empty path = empty state (no file loaded)
    explicit Document(const QString &path, QObject *parent = nullptr);
    ~Document() override;

    bool ok() const { return m_doc != nullptr; }
    QString fileName() const;
    QString filePath() const { return m_path; }

    int pageCount() const { return m_pageCount; }

    // Closes the current file (if any) and opens path. Returns false if the
    // file cannot be opened (state becomes: empty, no document).
    Q_INVOKABLE bool open(const QString &path);

    // Page size in PDF points. Thread-safe.
    Q_INVOKABLE QSizeF pageSizePt(int page) const;
    Q_INVOKABLE bool pageHasText(int page) const;
    Q_INVOKABLE bool pageHasImages(int page) const;

    // Printed page label ("iv", "40") as defined by the PDF; the plain 1-based
    // number when the document defines none. Thread-safe.
    // Per-file view state (page, offset within it, zoom, fit, rotation) kept in
    // QSettings so reopening a file lands where you left off. Empty map = none.
    Q_INVOKABLE QVariantMap loadViewState() const;
    Q_INVOKABLE void saveViewState(const QVariantMap &state) const;

    // Links on a page, loaded off the GUI thread: requestLinks() emits
    // linksReady(page, links); cachedLinks() returns what is loaded so far.
    // Each link: {x,y,w,h (page points), external, uri, page (0-based, -1 if
    // external), destY (page points, 0 if unspecified)}.
    Q_INVOKABLE QVariantList cachedLinks(int page) const;
    Q_INVOKABLE void requestLinks(int page);
    // Opens http/https/mailto links with the desktop; anything else is refused.
    Q_INVOKABLE bool openExternal(const QString &uri) const;

    Q_INVOKABLE QString pageLabel(int page) const;
    // 0-based page whose label equals `label` (case-insensitive), or -1.
    Q_INVOKABLE int pageForLabel(const QString &label) const;

    // Renders page at 1.0 = natural PDF size (one point per pixel); callers
    // factor in devicePixelRatio. Thread-safe, serialized.
    QImage renderPage(int page, qreal scale) const;
    QThreadPool *renderPool() const { return &m_renderPool; }

    // Worker-thread warm-up: renders then discards so MuPDF's store has the
    // page parsed and cached.
    Q_INVOKABLE void prefetch(int page, qreal scale) const;

    // Flat outline (table of contents): [{title, page(0-based, -1 if none), depth}]
    Q_INVOKABLE QVariantList outline() const;

    // First hit at or after startPage (wraps around). Returns
    // { page: int(-1 if none), boxes: [{x,y,w,h} in page points] }.
    Q_INVOKABLE QVariantMap searchFrom(int startPage, const QString &needle) const;
    Q_INVOKABLE QVariantList searchAll(const QString &needle) const;
    Q_INVOKABLE void searchAllAsync(const QString &needle, int generation);
    Q_INVOKABLE void cancelSearch();

    // Text selection between two page-space points. Returns
    // {text, boxes: [{x,y,w,h}]} in page coordinates.
    Q_INVOKABLE QVariantMap selectText(int page, QPointF anchor, QPointF focus) const;
    Q_INVOKABLE QVariantMap selectTextAt(int page, QPointF point,
                                         const QString &mode) const;
    Q_INVOKABLE QVariantMap selectTextRange(int anchorPage, QPointF anchor,
                                            int focusPage, QPointF focus) const;
    Q_INVOKABLE bool copyTextToClipboard(const QString &text) const;

    // Renders the page at 2x and puts it on the system clipboard.
    Q_INVOKABLE bool copyPageToClipboard(int page) const;

    bool isOpen() const { return m_doc != nullptr; }

signals:
    void linksReady(int page, const QVariantList &links);
    void pageCountChanged(int count);
    void searchFinished(int generation, const QVariantList &results);

private:
    void reset();
    bool load(const QString &path);
    void computeSizes();
    fz_stext_page *textPage(int page) const;
    QVariantList loadLinks(int page) const;
    mutable QHash<int, QVariantList> m_linkCache;
    mutable QMutex m_linkMutex;
    void ensureLabels() const;
    mutable QStringList m_labels;
    mutable bool m_labelsBuilt = false;
    QVariantMap selectTextStream(int page, QPointF anchor, QPointF focus) const;
    bool pageTextEndpoints(int page, QPointF &first, QPointF &last) const;

    fz_context *m_ctx = nullptr;
    fz_document *m_doc = nullptr;
    QString m_path;
    int m_pageCount = 0;
    QVector<QSizeF> m_sizes;
    QVector<QPointF> m_origins;
    mutable QHash<int, fz_stext_page *> m_textPages;
    mutable QMutex m_mutex;
    mutable QThreadPool m_renderPool;
    QThreadPool m_textPool;
    std::atomic<int> m_searchGeneration{0};
};
