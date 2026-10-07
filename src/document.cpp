#include "document.h"
#include "readingtext.h"

#include <QFileInfo>
#include <QThreadPool>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QImageReader>
#include <QStandardPaths>
#include <QUrl>
#include <QSettings>
#include <QClipboard>
#include <QDebug>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace {
QVariantMap quadBox(const fz_quad &q, const fz_rect &bounds)
{
    const float minx = std::min({q.ul.x, q.ur.x, q.ll.x, q.lr.x});
    const float maxx = std::max({q.ul.x, q.ur.x, q.ll.x, q.lr.x});
    const float miny = std::min({q.ul.y, q.ur.y, q.ll.y, q.lr.y});
    const float maxy = std::max({q.ul.y, q.ur.y, q.ll.y, q.lr.y});
    return {{QStringLiteral("x"), minx - bounds.x0},
            {QStringLiteral("y"), miny - bounds.y0},
            {QStringLiteral("w"), maxx - minx},
            {QStringLiteral("h"), maxy - miny}};
}

// Geometric same-column selection. MuPDF selects by text-stream index, which in
// tables and sidebars sweeps in unrelated cells interleaved between the two
// endpoints. When both endpoints sit in the same column we instead take the
// lines between them in visual order that overlap that column.
struct SelLine {
    fz_stext_line *line;
    fz_rect box;
    float midY;
};

std::vector<SelLine> horizontalLines(fz_stext_page *page)
{
    std::vector<SelLine> lines;
    for (fz_stext_block *block = page->first_block; block; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT)
            continue;
        for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
            if (!line->first_char || std::abs(line->dir.y) > 0.1f || line->dir.x <= 0)
                continue;
            lines.push_back({line, line->bbox, (line->bbox.y0 + line->bbox.y1) / 2});
        }
    }
    return lines;
}

int closestLine(const std::vector<SelLine> &lines, fz_point q)
{
    int best = -1;
    float bestV = 1e30f, bestH = 1e30f;
    for (int i = 0; i < int(lines.size()); ++i) {
        const fz_rect &b = lines[i].box;
        const float half = (b.y1 - b.y0) / 2;
        const float v = std::max(0.0f, std::abs(q.y - lines[i].midY) - half);
        const float h = q.x < b.x0 ? b.x0 - q.x : (q.x > b.x1 ? q.x - b.x1 : 0.0f);
        if (v < bestV - 0.01f || (std::abs(v - bestV) <= 0.01f && h < bestH)) {
            best = i;
            bestV = v;
            bestH = h;
        }
    }
    return best;
}

// Character boundary (0..n) in the line nearest to x.
int boundaryAt(fz_stext_line *line, float x)
{
    int idx = 0, best = 0;
    float bestD = 1e30f;
    for (fz_stext_char *ch = line->first_char; ch; ch = ch->next, ++idx) {
        const float d1 = std::abs(ch->quad.ll.x - x);
        const float d2 = std::abs(ch->quad.lr.x - x);
        if (d1 < bestD) { bestD = d1; best = idx; }
        if (d2 < bestD) { bestD = d2; best = idx + 1; }
    }
    return best;
}

// Selected text and box of chars [from, to) in the line.
void takeChars(fz_stext_line *line, int from, int to, QString &text, QVariantList &boxes,
               const fz_rect &bounds)
{
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    int idx = 0;
    for (fz_stext_char *ch = line->first_char; ch; ch = ch->next, ++idx) {
        if (idx < from || idx >= to)
            continue;
        text.append(QChar::fromUcs4(ch->c));
        const fz_quad &q = ch->quad;
        x0 = std::min({x0, q.ul.x, q.ur.x, q.ll.x, q.lr.x});
        x1 = std::max({x1, q.ul.x, q.ur.x, q.ll.x, q.lr.x});
        y0 = std::min({y0, q.ul.y, q.ur.y, q.ll.y, q.lr.y});
        y1 = std::max({y1, q.ul.y, q.ur.y, q.ll.y, q.lr.y});
    }
    if (x1 > x0)
        boxes.append(QVariantMap{{QStringLiteral("x"), x0 - bounds.x0},
                                 {QStringLiteral("y"), y0 - bounds.y0},
                                 {QStringLiteral("w"), x1 - x0},
                                 {QStringLiteral("h"), y1 - y0}});
}

// Double-click word: the identifier/punctuation run under the point, so code
// like "keras.layers.Input((128," selects "Input" rather than the whole chunk.
bool selectWordRun(fz_stext_page *page, fz_point q, const fz_rect &bounds, QString &text,
                   QVariantList &boxes)
{
    const std::vector<SelLine> lines = horizontalLines(page);
    const int li = closestLine(lines, q);
    if (li < 0)
        return false;
    fz_stext_line *line = lines[li].line;
    const fz_rect &b = lines[li].box;
    // Only when the pointer is actually on the line; otherwise let MuPDF snap.
    if (q.x < b.x0 - 4 || q.x > b.x1 + 4 || std::abs(q.y - lines[li].midY) > (b.y1 - b.y0))
        return false;
    std::vector<fz_stext_char *> chars;
    for (fz_stext_char *ch = line->first_char; ch; ch = ch->next)
        chars.push_back(ch);
    if (chars.empty())
        return false;
    int hit = 0;
    float best = 1e30f;
    for (int i = 0; i < int(chars.size()); ++i) {
        const float l = chars[i]->quad.ll.x, r = chars[i]->quad.lr.x;
        const float dx = q.x < l ? l - q.x : (q.x > r ? q.x - r : 0.0f);
        if (dx < best) { best = dx; hit = i; }
    }
    const auto cls = [&](int i) {
        const auto at = [&](int k) {
            return QChar(chars[k]->c < 0x10000 ? char16_t(chars[k]->c) : char16_t(0xFFFD));
        };
        const QChar c = at(i);
        const QChar prev = i > 0 ? at(i - 1) : QChar();
        const QChar next = i + 1 < int(chars.size()) ? at(i + 1) : QChar();
        if (c.isLetterOrNumber() || c == QLatin1Char('_'))
            return 1;
        if ((c == QLatin1Char('\'') || c == QChar(0x2019)) && prev.isLetter() && next.isLetter())
            return 1;
        if (c == QLatin1Char('.') && prev.isDigit() && next.isDigit())
            return 1;
        return c.isSpace() ? 0 : 2;
    };
    const int kind = cls(hit);
    int from = hit, to = hit;
    while (from > 0 && cls(from - 1) == kind)
        --from;
    while (to + 1 < int(chars.size()) && cls(to + 1) == kind)
        ++to;
    takeChars(line, from, to + 1, text, boxes, bounds);
    return !text.isEmpty();
}

// Returns false when the endpoints are not in one column (caller falls back to
// MuPDF's stream-order selection).
bool selectColumn(fz_stext_page *page, fz_point a, fz_point b, const fz_rect &bounds,
                  QString &text, QVariantList &boxes)
{
    const std::vector<SelLine> lines = horizontalLines(page);
    const int ia = closestLine(lines, a);
    const int ib = closestLine(lines, b);
    if (ia < 0 || ib < 0)
        return false;
    const fz_rect &ra = lines[ia].box;
    const fz_rect &rb = lines[ib].box;
    // Multi-column prose: several wide lines with another wide line beside them.
    const float wide = 0.3f * (bounds.x1 - bounds.x0);
    int sideBySide = 0;
    for (size_t i = 0; i < lines.size() && sideBySide < 4; ++i) {
        const fz_rect &p = lines[i].box;
        if (p.x1 - p.x0 <= wide)
            continue;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            const fz_rect &q = lines[j].box;
            if (q.x1 - q.x0 > wide && std::abs(lines[i].midY - lines[j].midY) < 3
                    && (p.x1 <= q.x0 || q.x1 <= p.x0)) {
                ++sideBySide;
                break;
            }
        }
    }
    const bool multiColumn = sideBySide >= 4;

    // Single-flow pages (including tables) are selected in plain visual order.
    // Only multi-column pages need the column band, so a drag in one column
    // doesn't pull in the other; cross-column drags use MuPDF's stream order.
    float bandX0 = -1e30f, bandX1 = 1e30f;
    if (multiColumn) {
        if (std::min(ra.x1, rb.x1) <= std::max(ra.x0, rb.x0))
            return false;
        bandX0 = std::min(ra.x0, rb.x0);
        bandX1 = std::max(ra.x1, rb.x1);
    }

    const auto before = [&](int i, int j) {
        return lines[i].midY < lines[j].midY - 0.5f
            || (std::abs(lines[i].midY - lines[j].midY) <= 0.5f
                && lines[i].box.x0 < lines[j].box.x0);
    };
    int top = ia, bottom = ib;
    fz_point pt = a, pb = b;
    if (ia != ib ? before(ib, ia) : b.x < a.x) {
        std::swap(top, bottom);
        std::swap(pt, pb);
    }

    if (top == bottom) {
        int s = boundaryAt(lines[top].line, pt.x), e = boundaryAt(lines[top].line, pb.x);
        if (s > e)
            std::swap(s, e);
        takeChars(lines[top].line, s, e, text, boxes, bounds);
        return true;
    }

    std::vector<int> order;
    for (int i = 0; i < int(lines.size()); ++i) {
        if (i == top || i == bottom)
            continue;
        if (before(top, i) && before(i, bottom)
                && std::min(lines[i].box.x1, bandX1) > std::max(lines[i].box.x0, bandX0))
            order.push_back(i);
    }
    std::sort(order.begin(), order.end(), before);

    const auto count = [](fz_stext_line *l) {
        int n = 0;
        for (fz_stext_char *c = l->first_char; c; c = c->next)
            ++n;
        return n;
    };
    QStringList parts;
    QString part;
    takeChars(lines[top].line, boundaryAt(lines[top].line, pt.x), count(lines[top].line),
              part, boxes, bounds);
    parts << part;
    for (int i : order) {
        part.clear();
        takeChars(lines[i].line, 0, count(lines[i].line), part, boxes, bounds);
        parts << part;
    }
    part.clear();
    takeChars(lines[bottom].line, 0, boundaryAt(lines[bottom].line, pb.x), part, boxes, bounds);
    parts << part;
    parts.removeAll(QString());
    text = parts.join(QLatin1Char('\n'));
    return true;
}

}

Document::Document(const QString &path, QObject *parent)
    : QObject(parent)
{
    m_renderPool.setMaxThreadCount(1);
    m_textPool.setMaxThreadCount(1);
    m_infoPool.setMaxThreadCount(1);
    m_changeTimer.setSingleShot(true);
    connect(&m_changeTimer, &QTimer::timeout, this, &Document::checkFileChange);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, this, [this] {
        watchFile();                 // a replaced file drops its watch; re-add
        m_changeRetries = 0;
        m_changeTimer.start(500);
    });
    connect(&m_fileWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        watchFile();
        m_changeRetries = 0;
        m_changeTimer.start(500);
    });
    if (!path.isEmpty())
        load(path);
}

Document::~Document()
{
    reset();
}

void Document::reset()
{
    m_textAbort = true;
    m_changeTimer.stop();
    if (!m_fileWatcher.files().isEmpty())
        m_fileWatcher.removePaths(m_fileWatcher.files());
    if (!m_fileWatcher.directories().isEmpty())
        m_fileWatcher.removePaths(m_fileWatcher.directories());
    m_labels.clear();
    m_labelsBuilt = false;
    m_labelsRequested = false;
    m_imageRectCache.clear();
    m_colorRectCache.clear();
    cancelSearch();
    m_textPool.waitForDone();
    m_infoPool.waitForDone();
    m_renderPool.waitForDone();
    {
        QMutexLocker lock(&m_infoMutex);
        m_pageInfoCache.clear();
        m_pageInfoPending.clear();
    }
    {
        QMutexLocker lock(&m_linkMutex);
        m_linkCache.clear();
    }
    for (fz_stext_page *page : std::as_const(m_textPages))
        fz_drop_stext_page(m_ctx, page);
    m_textPages.clear();
    m_sizes.clear();
    m_origins.clear();
    m_pageCount = 0;
    if (m_doc) {
        fz_drop_document(m_ctx, m_doc);
        m_doc = nullptr;
    }
    if (m_ctx) {
        fz_drop_context(m_ctx);
        m_ctx = nullptr;
    }
    m_path.clear();
    m_openPath.clear();
}

bool Document::openContainingFolder() const
{
    if (m_path.isEmpty())
        return false;
    return QDesktopServices::openUrl(
        QUrl::fromLocalFile(QFileInfo(m_path).absolutePath()));
}

void Document::watchFile()
{
    if (m_path.isEmpty())
        return;
    const QFileInfo info(m_path);
    // Watch the file and its folder: writers often replace the file (rename),
    // which silently ends a watch on the old inode.
    if (info.exists() && !m_fileWatcher.files().contains(m_path))
        m_fileWatcher.addPath(m_path);
    const QString dir = info.absolutePath();
    if (!m_fileWatcher.directories().contains(dir))
        m_fileWatcher.addPath(dir);
}

// Is `path` a document we can open right now? A separate MuPDF context so a
// half-written file can never disturb the document on screen.
static bool probeReadable(const QString &path)
{
    fz_context *ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx)
        return false;
    bool ok = false;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        fz_document *doc = fz_open_document(ctx, path.toUtf8().constData());
        ok = doc && fz_count_pages(ctx, doc) > 0;
        fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        ok = false;
    }
    fz_drop_context(ctx);
    if (!ok) {
        // Qt also has a PDF "image" plugin; a broken PDF must not count as readable.
        QImageReader reader(path);
        return reader.canRead() && reader.format().toLower() != "pdf";
    }
    // MuPDF happily "repairs" a truncated PDF, so a file still being written
    // would pass. A finished PDF ends with %%EOF; wait until it does.
    if (path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        f.seek(qMax<qint64>(0, f.size() - 2048));
        if (!f.readAll().contains("%%EOF"))
            return false;
    }
    return true;
}

// Debounced: only report a change once size and mtime stop moving (the writer
// is done) and the file opens; otherwise keep waiting, up to ~15 s.
void Document::checkFileChange()
{
    if (m_path.isEmpty())
        return;
    const QFileInfo info(m_path);
    if (!info.exists()) {
        if (++m_changeRetries < 30)
            m_changeTimer.start(500);
        return;
    }
    const qint64 size = info.size();
    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    if (size == m_loadedSize && mtime == m_loadedMtime)
        return;                                   // nothing really changed
    const bool stable = size == m_lastSeenSize && mtime == m_lastSeenMtime;
    m_lastSeenSize = size;
    m_lastSeenMtime = mtime;
    if (stable && size > 0 && probeReadable(m_path)) {
        emit fileChangedOnDisk();
        return;
    }
    if (++m_changeRetries < 30)
        m_changeTimer.start(500);
}

QVariantMap Document::computePageInfo(int pageNumber) const
{
    QVariantMap info;
    const bool hasText = pageHasText(pageNumber);
    QVariantList boxes;
    int chars = 0;
    if (hasText) {
        const QSizeF size = pageSizePt(pageNumber);
        const QVariantMap sel = const_cast<Document *>(this)->selectText(
            pageNumber, QPointF(0, 0), QPointF(size.width(), size.height()));
        chars = sel.value(QStringLiteral("text")).toString().size();
        boxes = sel.value(QStringLiteral("boxes")).toList();
    }
    const QVariantList rects = pageImageRects(pageNumber);
    info.insert(QStringLiteral("hasText"), hasText);
    info.insert(QStringLiteral("hasImages"), !rects.isEmpty() || pageHasImages(pageNumber));
    info.insert(QStringLiteral("chars"), chars);
    info.insert(QStringLiteral("boxes"), boxes);
    info.insert(QStringLiteral("imageRects"), rects);
    info.insert(QStringLiteral("colorRects"), pageColorRects(pageNumber));
    return info;
}

QVariantMap Document::cachedPageInfo(int page) const
{
    QMutexLocker lock(&m_infoMutex);
    return m_pageInfoCache.value(page);
}

QVariantMap Document::pageInfo(int page) const
{
    if (!isOpen() || page < 0 || page >= m_pageCount)
        return {};
    {
        QMutexLocker lock(&m_infoMutex);
        const auto it = m_pageInfoCache.constFind(page);
        if (it != m_pageInfoCache.constEnd())
            return it.value();
    }
    const QVariantMap info = computePageInfo(page);
    QMutexLocker lock(&m_infoMutex);
    m_pageInfoCache.insert(page, info);
    return info;
}

void Document::requestPageInfo(int page)
{
    if (!isOpen() || page < 0 || page >= m_pageCount)
        return;
    {
        QMutexLocker lock(&m_infoMutex);
        if (m_pageInfoCache.contains(page) || m_pageInfoPending.value(page))
            return;
        m_pageInfoPending.insert(page, true);
    }
    const QString path = m_path;
    m_infoPool.start([this, page, path] {
        if (path != m_path)
            return;
        const QVariantMap info = computePageInfo(page);
        {
            QMutexLocker lock(&m_infoMutex);
            m_pageInfoPending.remove(page);
            m_pageInfoCache.insert(page, info);
        }
        QMetaObject::invokeMethod(this, [this, page, path] {
            if (path == m_path)
                emit pageInfoReady(page);
        }, Qt::QueuedConnection);
    });
}

void Document::requestAllText()
{
    if (!isOpen())
        return;
    const QString path = m_path;
    const int generation = m_generation;
    const int pages = m_pageCount;
    m_textPool.start([this, path, generation, pages] {
        QStringList texts;
        texts.reserve(pages);
        for (int p = 0; p < pages; ++p) {
            if (m_textAbort || path != m_path)   // document closed or replaced
                return;
            const QSizeF size = pageSizePt(p);
            texts.append(selectText(p, QPointF(0, 0),
                                    QPointF(size.width(), size.height()))
                             .value(QStringLiteral("text")).toString());
        }
        QMetaObject::invokeMethod(this, [this, generation, texts, path] {
            if (path == m_path)
                emit allTextReady(generation, texts);
        }, Qt::QueuedConnection);
    });
}

bool Document::open(const QString &path)
{
    reset();
    if (!load(path)) {
        emit pageCountChanged(0);
        return false;
    }
    return true;
}

QString Document::fileName() const
{
    return QFileInfo(m_path).fileName();
}

namespace {
// Formats MuPDF cannot decode but Qt's image plugins can (WebP, ICO, TGA...) are
// converted once to a PNG in the cache, keyed by the original file so OCR
// results cached against the PNG stay valid between sessions.
QString convertWithQt(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead() || reader.format().toLower() == "pdf")
        return {};
    const QFileInfo info(path);
    const QString key = QString::fromLatin1(QCryptographicHash::hash(
        QStringLiteral("%1|%2|%3").arg(info.canonicalFilePath()).arg(info.size())
            .arg(info.lastModified().toMSecsSinceEpoch()).toUtf8(),
        QCryptographicHash::Sha1).toHex());
    const QString dirPath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/converted");
    QDir dir(dirPath);
    if (!dir.mkpath(QStringLiteral(".")))
        return {};
    const QString out = dir.filePath(key + QStringLiteral(".png"));
    if (!QFileInfo::exists(out)) {
        const QImage image = reader.read();   // first frame of an animation
        if (image.isNull() || !image.save(out, "PNG"))
            return {};
        // Keep the cache small: drop the oldest conversions beyond 40.
        QFileInfoList files = dir.entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Time);
        for (int i = 40; i < files.size(); ++i)
            QFile::remove(files.at(i).absoluteFilePath());
    }
    return out;
}
}

bool Document::load(const QString &path)
{
    m_path = path;
    ++m_generation;
    m_textAbort = false;

    m_ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!m_ctx) {
        qWarning() << "glance: fz_new_context failed";
        m_path.clear();
        return false;
    }

    fz_try(m_ctx)
        fz_register_document_handlers(m_ctx);
    fz_catch(m_ctx) {
        qWarning() << "glance: cannot register document handlers";
        fz_drop_context(m_ctx);
        m_ctx = nullptr;
        m_path.clear();
        return false;
    }

    m_openPath.clear();
    bool opened = false;
    fz_try(m_ctx)
        m_doc = fz_open_document(m_ctx, path.toUtf8().constData());
    fz_catch(m_ctx)
        opened = false;
    if (m_doc)
        opened = true;
    if (!opened) {
        // MuPDF could not read it; try Qt's image plugins (WebP, ICO, TGA...).
        const QString converted = convertWithQt(path);
        if (!converted.isEmpty()) {
            fz_try(m_ctx)
                m_doc = fz_open_document(m_ctx, converted.toUtf8().constData());
            fz_catch(m_ctx)
                m_doc = nullptr;
            if (m_doc) {
                m_openPath = converted;
                opened = true;
            }
        }
    }
    if (!opened) {
        qWarning() << "glance: cannot open" << path;
        fz_drop_context(m_ctx);
        m_ctx = nullptr;
        m_path.clear();
        return false;
    }

    computeSizes();
    const QFileInfo info(m_path);
    m_loadedSize = m_lastSeenSize = info.size();
    m_loadedMtime = m_lastSeenMtime = info.lastModified().toMSecsSinceEpoch();
    watchFile();
    return true;
}

void Document::computeSizes()
{
    QMutexLocker lock(&m_mutex);
    m_pageCount = fz_count_pages(m_ctx, m_doc);
    m_sizes.reserve(m_pageCount);
    for (int i = 0; i < m_pageCount; ++i) {
        fz_try(m_ctx) {
            fz_page *page = fz_load_page(m_ctx, m_doc, i);
            fz_rect bounds = fz_bound_page(m_ctx, page);
            fz_drop_page(m_ctx, page);
            m_sizes.append(QSizeF(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0));
            m_origins.append(QPointF(bounds.x0, bounds.y0));
        }
        fz_catch(m_ctx) {
            m_sizes.append(QSizeF(612, 792)); // US Letter fallback
            m_origins.append(QPointF(0, 0));
        }
    }
    lock.unlock();
    emit pageCountChanged(m_pageCount);
}

QSizeF Document::pageSizePt(int page) const
{
    // No lock: the sizes are filled in on load and only cleared by reset(),
    // which both run on the GUI thread after every worker has finished.
    if (page < 0 || page >= m_sizes.size())
        return QSizeF(612, 792);
    return m_sizes.at(page);
}

bool Document::pageHasText(int pageNumber) const
{
    if (!isOpen() || pageNumber < 0 || pageNumber >= m_pageCount)
        return false;
    QMutexLocker lock(&m_mutex);
    fz_stext_page *page = nullptr;
    bool found = false;
    // Never `return` from inside fz_try: it leaks an exception-stack frame, and
    // after enough of them every MuPDF call fails ("exception stack overflow").
    fz_try(m_ctx) {
        page = textPage(pageNumber);
        for (fz_stext_block *block = page->first_block; block && !found; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
                if (line->first_char) {
                    found = true;
                    break;
                }
            }
        }
    }
    fz_catch(m_ctx) {
        return false;
    }
    return found;
}

namespace {
constexpr int kMaxRememberedFiles = 200;

QString viewStateKey(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return QString::fromLatin1(QCryptographicHash::hash(
        (canonical.isEmpty() ? path : canonical).toUtf8(),
        QCryptographicHash::Sha1).toHex());
}
}

QVariantMap Document::loadViewState() const
{
    if (!isOpen())
        return {};
    QSettings settings;
    settings.beginGroup(QStringLiteral("view/") + viewStateKey(m_path));
    if (!settings.contains(QStringLiteral("page")))
        return {};
    // A different page count means the file changed underneath us.
    if (settings.value(QStringLiteral("pageCount")).toInt() != m_pageCount)
        return {};
    QVariantMap state;
    state.insert(QStringLiteral("page"), settings.value(QStringLiteral("page")).toInt());
    state.insert(QStringLiteral("offset"), settings.value(QStringLiteral("offset")).toDouble());
    state.insert(QStringLiteral("zoom"), settings.value(QStringLiteral("zoom")).toDouble());
    state.insert(QStringLiteral("fit"), settings.value(QStringLiteral("fit")).toBool());
    state.insert(QStringLiteral("rotation"), settings.value(QStringLiteral("rotation")).toInt());
    state.insert(QStringLiteral("perRow"), settings.value(QStringLiteral("perRow"), 1).toInt());
    state.insert(QStringLiteral("cover"), settings.value(QStringLiteral("cover"), true).toBool());
    return state;
}

void Document::saveViewState(const QVariantMap &state) const
{
    if (!isOpen() || m_pageCount <= 0)
        return;
    QSettings settings;
    const QString key = viewStateKey(m_path);
    settings.beginGroup(QStringLiteral("view/") + key);
    for (auto it = state.cbegin(); it != state.cend(); ++it)
        settings.setValue(it.key(), it.value());
    settings.setValue(QStringLiteral("pageCount"), m_pageCount);
    settings.setValue(QStringLiteral("touched"), QDateTime::currentMSecsSinceEpoch());
    settings.endGroup();

    // Keep the most recently used files only.
    settings.beginGroup(QStringLiteral("view"));
    const QStringList groups = settings.childGroups();
    if (groups.size() > kMaxRememberedFiles) {
        QList<QPair<qint64, QString>> byAge;
        for (const QString &g : groups)
            byAge.append({settings.value(g + QStringLiteral("/touched")).toLongLong(), g});
        std::sort(byAge.begin(), byAge.end());
        for (int i = 0; i < groups.size() - kMaxRememberedFiles; ++i)
            settings.remove(byAge.at(i).second);
    }
}

QVariantList Document::loadLinks(int pageNumber) const
{
    QVariantList result;
    QMutexLocker lock(&m_mutex);
    if (!isOpen() || pageNumber < 0 || pageNumber >= m_pageCount)
        return result;
    fz_page *page = nullptr;
    fz_link *links = nullptr;
    fz_try(m_ctx) {
        const QPointF origin = m_origins.at(pageNumber);
        page = fz_load_page(m_ctx, m_doc, pageNumber);
        links = fz_load_links(m_ctx, page);
        for (fz_link *link = links; link; link = link->next) {
            if (!link->uri)
                continue;
            QVariantMap entry{
                {QStringLiteral("x"), link->rect.x0 - origin.x()},
                {QStringLiteral("y"), link->rect.y0 - origin.y()},
                {QStringLiteral("w"), link->rect.x1 - link->rect.x0},
                {QStringLiteral("h"), link->rect.y1 - link->rect.y0},
                {QStringLiteral("uri"), QString::fromUtf8(link->uri)},
                {QStringLiteral("external"), bool(fz_is_external_link(m_ctx, link->uri))},
                {QStringLiteral("page"), -1},
                {QStringLiteral("destY"), 0.0}};
            if (!entry.value(QStringLiteral("external")).toBool()) {
                fz_link_dest dest = fz_resolve_link_dest(m_ctx, m_doc, link->uri);
                const int target = fz_page_number_from_location(m_ctx, m_doc, dest.loc);
                if (target < 0 || target >= m_pageCount)
                    continue;
                entry[QStringLiteral("page")] = target;
                if (std::isfinite(dest.y))
                    entry[QStringLiteral("destY")] = dest.y - m_origins.at(target).y();
            }
            result.append(entry);
        }
    }
    fz_always(m_ctx) {
        fz_drop_link(m_ctx, links);
        fz_drop_page(m_ctx, page);
    }
    fz_catch(m_ctx) {
        qWarning() << "glance: could not load links on page" << pageNumber;
    }
    return result;
}

QVariantList Document::cachedLinks(int page) const
{
    QMutexLocker lock(&m_linkMutex);
    return m_linkCache.value(page);
}

void Document::requestLinks(int page)
{
    {
        QMutexLocker lock(&m_linkMutex);
        if (m_linkCache.contains(page))
            return;
        m_linkCache.insert(page, {});   // claim it so duplicate requests are dropped
    }
    const QString path = m_path;
    m_textPool.start([this, page, path] {
        const QVariantList links = loadLinks(page);
        {
            QMutexLocker lock(&m_linkMutex);
            m_linkCache.insert(page, links);
        }
        if (!links.isEmpty())
            QMetaObject::invokeMethod(this, [this, page, links, path] {
                if (path == m_path)
                    emit linksReady(page, links);
            }, Qt::QueuedConnection);
    });
}

bool Document::openExternal(const QString &uri) const
{
    const QUrl url(uri);
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
            && scheme != QLatin1String("mailto"))
        return false;
    return QDesktopServices::openUrl(url);
}

// Builds every label under the MuPDF lock. Runs on the info pool (or inline for
// an explicit go-to) so the header never waits on it.
void Document::ensureLabels() const
{
    QMutexLocker lock(&m_mutex);
    if (m_labelsBuilt)
        return;
    QStringList labels;
    labels.reserve(m_pageCount);
    for (int i = 0; i < m_pageCount; ++i) {
        QString label = QString::number(i + 1);
        fz_page *page = nullptr;
        fz_try(m_ctx) {
            page = fz_load_page(m_ctx, m_doc, i);
            char buf[64] = {0};
            fz_page_label(m_ctx, page, buf, sizeof buf);
            if (buf[0])
                label = QString::fromUtf8(buf);
        }
        fz_always(m_ctx) {
            fz_drop_page(m_ctx, page);
        }
        fz_catch(m_ctx) {
        }
        labels.append(label);
    }
    m_labels = labels;
    m_labelsBuilt = true;
}

QString Document::pageLabel(int page) const
{
    if (!isOpen() || page < 0 || page >= m_pageCount)
        return QString();
    if (!m_labelsBuilt) {
        // Build in the background and report back; show the plain number meanwhile.
        if (!m_labelsRequested.exchange(true)) {
            Document *self = const_cast<Document *>(this);
            const QString path = m_path;
            self->m_infoPool.start([self, path] {
                self->ensureLabels();
                QMetaObject::invokeMethod(self, [self, path] {
                    if (path == self->m_path) {
                        ++self->m_labelsVersion;
                        emit self->labelsReady();
                    }
                }, Qt::QueuedConnection);
            });
        }
        return QString();
    }
    return m_labels.value(page);
}

int Document::pageForLabel(const QString &label) const
{
    if (!isOpen())
        return -1;
    ensureLabels();
    const QString wanted = label.trimmed();
    for (int i = 0; i < m_labels.size(); ++i)
        if (m_labels.at(i).compare(wanted, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

bool Document::pageHasImages(int pageNumber) const
{
    if (!isOpen() || pageNumber < 0 || pageNumber >= m_pageCount)
        return false;
    QMutexLocker lock(&m_mutex);
    fz_stext_page *text = nullptr;
    bool found = false;
    fz_try(m_ctx) {
        fz_stext_options options{};
        options.flags = FZ_STEXT_PRESERVE_IMAGES;
        text = fz_new_stext_page_from_page_number(m_ctx, m_doc, pageNumber, &options);
        for (fz_stext_block *block = text->first_block; block; block = block->next) {
            if (block->type == FZ_STEXT_BLOCK_IMAGE) {
                found = true;
                break;
            }
        }
    }
    fz_always(m_ctx) {
        fz_drop_stext_page(m_ctx, text);
    }
    fz_catch(m_ctx) {
        return false;
    }
    return found;
}

QVariantList Document::pageImageRects(int pageNumber) const
{
    if (!isOpen() || pageNumber < 0 || pageNumber >= m_pageCount)
        return {};
    QMutexLocker lock(&m_mutex);
    if (m_imageRectCache.contains(pageNumber))
        return m_imageRectCache.value(pageNumber);

    std::vector<QRectF> found;
    fz_stext_page *text = nullptr;
    fz_try(m_ctx) {
        const QPointF origin = m_origins.at(pageNumber);
        const QSizeF size = m_sizes.at(pageNumber);
        fz_stext_options options{};
        options.flags = FZ_STEXT_PRESERVE_IMAGES;
        text = fz_new_stext_page_from_page_number(m_ctx, m_doc, pageNumber, &options);
        const double pageArea = std::max(1.0, size.width() * size.height());
        for (fz_stext_block *block = text->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_IMAGE)
                continue;
            QRectF r(block->bbox.x0 - origin.x(), block->bbox.y0 - origin.y(),
                     block->bbox.x1 - block->bbox.x0, block->bbox.y1 - block->bbox.y0);
            // Skip icons, rules and bullets: too small to hold readable text.
            if (r.width() < 36 || r.height() < 14 || r.width() * r.height() < 0.02 * pageArea)
                continue;
            r.adjust(-4, -4, 4, 4);
            r = r.intersected(QRectF(QPointF(0, 0), size));
            if (!r.isEmpty())
                found.push_back(r);
        }
    }
    fz_always(m_ctx) {
        fz_drop_stext_page(m_ctx, text);
    }
    fz_catch(m_ctx) {
        found.clear();
    }
    std::sort(found.begin(), found.end(), [](const QRectF &a, const QRectF &b) {
        return a.width() * a.height() > b.width() * b.height();
    });
    QVariantList rects;
    for (size_t i = 0; i < found.size() && i < 6; ++i)
        rects.append(QVariantMap{{QStringLiteral("x"), found[i].x()},
                                 {QStringLiteral("y"), found[i].y()},
                                 {QStringLiteral("w"), found[i].width()},
                                 {QStringLiteral("h"), found[i].height()}});
    m_imageRectCache.insert(pageNumber, rects);
    return rects;
}

QVariantList Document::pageColorRects(int pageNumber) const
{
    if (!isOpen() || pageNumber < 0 || pageNumber >= m_pageCount)
        return {};
    QMutexLocker lock(&m_mutex);
    if (m_colorRectCache.contains(pageNumber))
        return m_colorRectCache.value(pageNumber);

    QVariantList rects;
    fz_stext_page *text = nullptr;
    fz_try(m_ctx) {
        const QPointF origin = m_origins.at(pageNumber);
        const QSizeF size = m_sizes.at(pageNumber);
        fz_stext_options options{};
        options.flags = FZ_STEXT_PRESERVE_IMAGES;
        text = fz_new_stext_page_from_page_number(m_ctx, m_doc, pageNumber, &options);
        const double pageArea = std::max(1.0, size.width() * size.height());
        for (fz_stext_block *block = text->first_block; block && rects.size() < 60;
             block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_IMAGE)
                continue;
            QRectF r(block->bbox.x0 - origin.x(), block->bbox.y0 - origin.y(),
                     block->bbox.x1 - block->bbox.x0, block->bbox.y1 - block->bbox.y0);
            r = r.intersected(QRectF(QPointF(0, 0), size));
            // A page-sized picture is a scan: it is the page, so recolour it.
            if (r.width() < 6 || r.height() < 6 || r.width() * r.height() > 0.85 * pageArea)
                continue;
            rects.append(QVariantMap{{QStringLiteral("x"), r.x()},
                                     {QStringLiteral("y"), r.y()},
                                     {QStringLiteral("w"), r.width()},
                                     {QStringLiteral("h"), r.height()}});
        }
    }
    fz_always(m_ctx) {
        fz_drop_stext_page(m_ctx, text);
    }
    fz_catch(m_ctx) {
        rects.clear();
    }
    m_colorRectCache.insert(pageNumber, rects);
    return rects;
}

QImage Document::renderPage(int page, qreal scale) const
{
    if (!isOpen() || scale <= 0)
        return QImage();

    QMutexLocker lock(&m_mutex);
    fz_pixmap *pix = nullptr;
    fz_try(m_ctx) {
        fz_matrix ctm = fz_scale(scale, scale);
        pix = fz_new_pixmap_from_page_number(m_ctx, m_doc, page, ctm, fz_device_rgb(m_ctx), 0);
    }
    fz_catch(m_ctx) {
        qWarning() << "glance: render failed on page" << page;
        return QImage();
    }

    if (!pix || pix->w == 0 || pix->h == 0) {
        if (pix)
            fz_drop_pixmap(m_ctx, pix);
        return QImage();
    }

    QImage img(pix->samples, pix->w, pix->h, static_cast<qsizetype>(pix->stride),
               QImage::Format_RGB888);
    QImage copy = img.copy(); // detach before dropping the pixmap
    fz_drop_pixmap(m_ctx, pix);
    return copy;
}

// A crop of a page, in page points relative to the page origin, at `scale`.
QImage Document::renderRegion(int page, qreal scale, const QRectF &region) const
{
    if (!isOpen() || scale <= 0 || page < 0 || page >= m_pageCount || region.isEmpty())
        return QImage();
    const int w = std::clamp(int(std::ceil(region.width() * scale)), 1, 8192);
    const int h = std::clamp(int(std::ceil(region.height() * scale)), 1, 8192);

    QMutexLocker lock(&m_mutex);
    fz_pixmap *pix = nullptr;
    fz_device *dev = nullptr;
    fz_page *fzPage = nullptr;
    QImage copy;
    fz_var(pix);
    fz_var(dev);
    fz_var(fzPage);
    fz_try(m_ctx) {
        const QPointF origin = m_origins.at(page);
        fz_matrix ctm = fz_concat(fz_translate(-float(origin.x() + region.x()),
                                               -float(origin.y() + region.y())),
                                  fz_scale(scale, scale));
        fzPage = fz_load_page(m_ctx, m_doc, page);
        pix = fz_new_pixmap_with_bbox(m_ctx, fz_device_rgb(m_ctx), fz_make_irect(0, 0, w, h), nullptr, 0);
        fz_clear_pixmap_with_value(m_ctx, pix, 0xff);
        dev = fz_new_draw_device(m_ctx, fz_identity, pix);
        fz_run_page(m_ctx, fzPage, dev, ctm, nullptr);
        fz_close_device(m_ctx, dev);
        QImage img(pix->samples, pix->w, pix->h, static_cast<qsizetype>(pix->stride),
                   QImage::Format_RGB888);
        copy = img.copy();
    }
    fz_always(m_ctx) {
        fz_drop_device(m_ctx, dev);
        fz_drop_pixmap(m_ctx, pix);
        fz_drop_page(m_ctx, fzPage);
    }
    fz_catch(m_ctx) {
        qWarning() << "glance: region render failed on page" << page;
        return QImage();
    }
    return copy;
}

void Document::prefetch(int page, qreal scale) const
{
    if (!isOpen())
        return;

    m_renderPool.start([this, page, scale]() {
        // Out-of-range pages are silently skipped; also catches -1.
        if (page < 0 || page >= m_pageCount)
            return;
        renderPage(page, scale); // result discarded; warms the MuPDF store
    });
}

QVariantList Document::outline() const
{
    QVariantList out;
    if (!isOpen())
        return out;

    QMutexLocker lock(&m_mutex);
    fz_outline *root = nullptr;
    fz_try(m_ctx)
        root = fz_load_outline(m_ctx, m_doc);
    fz_catch(m_ctx) {
        return out;
    }

    std::function<void(fz_outline *, int)> walk = [&](fz_outline *o, int depth) {
        for (; o; o = o->next) {
            QVariantMap m;
            m.insert(QStringLiteral("title"),
                     QString::fromUtf8(o->title ? o->title : ""));
            // fz_location: chapter ignored (PDF); page is 0-based, -1 if none
            m.insert(QStringLiteral("page"), o->page.page);
            m.insert(QStringLiteral("depth"), depth);
            out.append(m);
            if (o->down)
                walk(o->down, depth + 1);
        }
    };
    walk(root, 0);
    fz_drop_outline(m_ctx, root);
    return out;
}

QVariantMap Document::searchFrom(int startPage, const QString &needle) const
{
    QVariantMap res;
    res.insert(QStringLiteral("page"), -1);
    res.insert(QStringLiteral("boxes"), QVariantList());

    if (!isOpen() || needle.isEmpty() || m_pageCount <= 0)
        return res;

    QMutexLocker lock(&m_mutex);
    const QByteArray nd = needle.toUtf8();
    const int n = m_pageCount;
    if (startPage < 0)
        startPage = 0;

    for (int i = 0; i < n; ++i) {
        const int p = (startPage + i) % n;
        fz_quad quads[64];
        int hits = 0;
        fz_rect bounds = fz_make_rect(0, 0, 0, 0);

        fz_try(m_ctx) {
            const QPointF origin = m_origins.at(p);
            const QSizeF size = m_sizes.at(p);
            bounds = fz_make_rect(origin.x(), origin.y(),
                                  origin.x() + size.width(), origin.y() + size.height());
            hits = fz_search_page_number(m_ctx, m_doc, p, nd.constData(),
                                         nullptr, quads, 64);
        }
        fz_catch(m_ctx) {
            continue;
        }

        if (hits <= 0)
            continue;

        QVariantList boxes;
        for (int k = 0; k < hits; ++k)
            boxes.append(quadBox(quads[k], bounds));
        res.insert(QStringLiteral("page"), p);
        res.insert(QStringLiteral("boxes"), boxes);
        return res;
    }

    return res;
}

QVariantList Document::searchAll(const QString &needle) const
{
    QVariantList results;
    if (!isOpen() || needle.isEmpty())
        return results;

    QMutexLocker lock(&m_mutex);
    const QByteArray nd = needle.toUtf8();
    for (int page = 0; page < m_pageCount; ++page) {
        fz_quad quads[256];
        int hits = 0;
        fz_try(m_ctx) {
            hits = fz_search_page_number(m_ctx, m_doc, page, nd.constData(),
                                         nullptr, quads, 256);
        }
        fz_catch(m_ctx) {
            continue;
        }
        if (hits <= 0)
            continue;

        const QPointF origin = m_origins.at(page);
        const QSizeF size = m_sizes.at(page);
        const fz_rect bounds = fz_make_rect(origin.x(), origin.y(),
                                            origin.x() + size.width(),
                                            origin.y() + size.height());
        QVariantList boxes;
        for (int i = 0; i < hits; ++i)
            boxes.append(quadBox(quads[i], bounds));
        results.append(QVariantMap{
            {QStringLiteral("page"), page},
            {QStringLiteral("boxes"), boxes},
            {QStringLiteral("count"), hits}
        });
    }
    return results;
}

void Document::searchAllAsync(const QString &needle, int generation)
{
    m_searchGeneration.store(generation);
    m_textPool.start([this, needle, generation] {
        QVariantList results;
        if (!isOpen() || needle.isEmpty())
            return;
        const QByteArray nd = needle.toUtf8();
        for (int page = 0; page < m_pageCount; ++page) {
            if (m_searchGeneration.load() != generation)
                return;
            fz_quad quads[256];
            int hits = 0;
            QPointF origin;
            QSizeF size;
            {
                QMutexLocker lock(&m_mutex);
                origin = m_origins.at(page);
                size = m_sizes.at(page);
                fz_try(m_ctx) {
                    hits = fz_search_page_number(m_ctx, m_doc, page, nd.constData(),
                                                 nullptr, quads, 256);
                }
                fz_catch(m_ctx) {
                    continue;
                }
            }
            if (hits <= 0)
                continue;
            const fz_rect bounds = fz_make_rect(origin.x(), origin.y(),
                                                origin.x() + size.width(),
                                                origin.y() + size.height());
            QVariantList boxes;
            for (int i = 0; i < hits; ++i)
                boxes.append(quadBox(quads[i], bounds));
            results.append(QVariantMap{
                {QStringLiteral("page"), page},
                {QStringLiteral("boxes"), boxes},
                {QStringLiteral("count"), hits}
            });
        }
        if (m_searchGeneration.load() != generation)
            return;
        QMetaObject::invokeMethod(this, [this, generation, results] {
            emit searchFinished(generation, results);
        }, Qt::QueuedConnection);
    });
}

void Document::cancelSearch()
{
    m_searchGeneration.fetch_add(1);
    m_textPool.clear();
}

QVariantMap Document::selectText(int pageNumber, QPointF anchor, QPointF focus) const
{
    QVariantMap result{{QStringLiteral("text"), QString()},
                       {QStringLiteral("boxes"), QVariantList()}};
    if (!isOpen() || pageNumber < 0 || pageNumber >= m_pageCount)
        return result;
    {
        QMutexLocker lock(&m_mutex);
        QString text;
        QVariantList boxes;
        bool done = false;
        fz_try(m_ctx) {
            const QPointF origin = m_origins.at(pageNumber);
            const QSizeF size = m_sizes.at(pageNumber);
            const fz_rect bounds = fz_make_rect(origin.x(), origin.y(),
                origin.x() + size.width(), origin.y() + size.height());
            done = selectColumn(textPage(pageNumber),
                                fz_make_point(anchor.x() + bounds.x0, anchor.y() + bounds.y0),
                                fz_make_point(focus.x() + bounds.x0, focus.y() + bounds.y0),
                                bounds, text, boxes);
        }
        fz_catch(m_ctx) {
            done = false;
        }
        if (done) {
            result.insert(QStringLiteral("text"), text);
            result.insert(QStringLiteral("boxes"), boxes);
            return result;
        }
    }
    return selectTextStream(pageNumber, anchor, focus);
}

QVariantMap Document::selectTextStream(int pageNumber, QPointF anchor, QPointF focus) const
{
    QVariantMap result{{QStringLiteral("text"), QString()},
                       {QStringLiteral("boxes"), QVariantList()}};
    if (!isOpen() || pageNumber < 0 || pageNumber >= m_pageCount)
        return result;

    QMutexLocker lock(&m_mutex);
    fz_stext_page *structuredText = nullptr;
    char *text = nullptr;
    fz_rect bounds = fz_make_rect(0, 0, 0, 0);
    fz_try(m_ctx) {
        const QPointF origin = m_origins.at(pageNumber);
        const QSizeF size = m_sizes.at(pageNumber);
        bounds = fz_make_rect(origin.x(), origin.y(),
                              origin.x() + size.width(), origin.y() + size.height());
        structuredText = textPage(pageNumber);

        fz_point a = fz_make_point(anchor.x() + bounds.x0, anchor.y() + bounds.y0);
        fz_point b = fz_make_point(focus.x() + bounds.x0, focus.y() + bounds.y0);
        fz_snap_selection(m_ctx, structuredText, &a, &b, FZ_SELECT_CHARS);

        // MuPDF returns only the number written, not the required capacity.
        // One merged quad per line is typical; 4096 safely covers dense pages.
        std::vector<fz_quad> quads(4096);
        const int written = fz_highlight_selection(
            m_ctx, structuredText, a, b, quads.data(), int(quads.size()));
        QVariantList boxes;
        for (int i = 0; i < written; ++i)
            boxes.append(quadBox(quads[i], bounds));

        text = fz_copy_selection(m_ctx, structuredText, a, b, 0);
        result.insert(QStringLiteral("text"), QString::fromUtf8(text ? text : ""));
        result.insert(QStringLiteral("boxes"), boxes);
    }
    fz_always(m_ctx) {
        fz_free(m_ctx, text);
    }
    fz_catch(m_ctx) {
        qWarning() << "glance: text selection failed on page" << pageNumber;
    }
    return result;
}

QVariantMap Document::selectTextAt(int pageNumber, QPointF point,
                                   const QString &mode) const
{
    return selectTextAtImpl(pageNumber, point, mode, true);
}

QVariantMap Document::hoverTextAt(int pageNumber, QPointF point) const
{
    return selectTextAtImpl(pageNumber, point, QStringLiteral("word"), false);
}

QVariantMap Document::selectTextAtImpl(int pageNumber, QPointF point,
                                       const QString &mode, bool wait) const
{
    QVariantMap result{{QStringLiteral("text"), QString()},
                       {QStringLiteral("boxes"), QVariantList()}};
    if (!isOpen() || pageNumber < 0 || pageNumber >= m_pageCount)
        return result;

    // The hover path must never stall the GUI: give up if a render holds the
    // lock, or if this page's text has not been extracted yet (warm it instead).
    if (wait) {
        m_mutex.lock();
    } else if (!m_mutex.tryLock()) {
        result.insert(QStringLiteral("busy"), true);
        return result;
    }
    struct Unlock { QMutex *m; ~Unlock() { m->unlock(); } } unlock{&m_mutex};
    if (!wait && !m_textPages.contains(pageNumber)) {
        result.insert(QStringLiteral("busy"), true);
        const_cast<Document *>(this)->requestPageInfo(pageNumber);   // queues; cheap
        return result;
    }
    char *text = nullptr;
    bool handled = false;   // never `return` inside fz_try: it leaks a stack frame
    fz_try(m_ctx) {
        const QPointF origin = m_origins.at(pageNumber);
        const QSizeF size = m_sizes.at(pageNumber);
        const fz_rect bounds = fz_make_rect(
            origin.x(), origin.y(), origin.x() + size.width(), origin.y() + size.height());
        fz_stext_page *structuredText = textPage(pageNumber);
        fz_point a = fz_make_point(point.x() + bounds.x0, point.y() + bounds.y0);
        fz_point b = a;
        if (mode != QStringLiteral("line")) {
            QString runText;
            QVariantList runBoxes;
            if (selectWordRun(structuredText, a, bounds, runText, runBoxes)) {
                result.insert(QStringLiteral("text"), runText);
                result.insert(QStringLiteral("boxes"), runBoxes);
                handled = true;
            }
        }
        if (!handled) {
        const int snapMode = mode == QStringLiteral("line")
            ? FZ_SELECT_LINES : FZ_SELECT_WORDS;
        fz_snap_selection(m_ctx, structuredText, &a, &b, snapMode);

        std::vector<fz_quad> quads(4096);
        const int written = fz_highlight_selection(
            m_ctx, structuredText, a, b, quads.data(), int(quads.size()));
        QVariantList boxes;
        for (int i = 0; i < written; ++i)
            boxes.append(quadBox(quads[i], bounds));
        text = fz_copy_selection(m_ctx, structuredText, a, b, 0);
        result.insert(QStringLiteral("text"), QString::fromUtf8(text ? text : ""));
        result.insert(QStringLiteral("boxes"), boxes);
        }
    }
    fz_always(m_ctx) {
        fz_free(m_ctx, text);
    }
    fz_catch(m_ctx) {
        qWarning() << "glance: point selection failed on page" << pageNumber;
    }
    return result;
}

QVariantMap Document::selectTextRange(int anchorPage, QPointF anchor,
                                      int focusPage, QPointF focus) const
{
    QVariantMap result{{QStringLiteral("text"), QString()},
                       {QStringLiteral("pages"), QVariantList()}};
    if (!isOpen() || anchorPage < 0 || focusPage < 0
            || anchorPage >= m_pageCount || focusPage >= m_pageCount)
        return result;

    if (anchorPage > focusPage) {
        std::swap(anchorPage, focusPage);
        std::swap(anchor, focus);
    } else if (anchorPage == focusPage
               && (anchor.y() > focus.y()
                   || (qFuzzyCompare(anchor.y(), focus.y()) && anchor.x() > focus.x()))) {
        std::swap(anchor, focus);
    }

    QStringList textParts;
    QVariantList pages;
    for (int page = anchorPage; page <= focusPage; ++page) {
        QPointF pageFirst;
        QPointF pageLast;
        if (!pageTextEndpoints(page, pageFirst, pageLast)) {
            pages.append(QVariantMap{
                {QStringLiteral("page"), page},
                {QStringLiteral("boxes"), QVariantList()}
            });
            continue;
        }
        const QPointF start = page == anchorPage ? anchor : pageFirst;
        const QPointF end = page == focusPage ? focus : pageLast;
        const QVariantMap selection = anchorPage == focusPage
            ? selectText(page, start, end) : selectTextStream(page, start, end);
        const QString pageText = selection.value(QStringLiteral("text")).toString();
        if (!pageText.isEmpty())
            textParts.append(pageText);
        pages.append(QVariantMap{
            {QStringLiteral("page"), page},
            {QStringLiteral("boxes"), selection.value(QStringLiteral("boxes"))}
        });
    }
    result.insert(QStringLiteral("text"), textParts.join(QLatin1Char('\n')));
    result.insert(QStringLiteral("pages"), pages);
    return result;
}

bool Document::pageTextEndpoints(int pageNumber, QPointF &first, QPointF &last) const
{
    QMutexLocker lock(&m_mutex);
    fz_stext_page *page = nullptr;
    fz_stext_char *firstChar = nullptr;
    fz_stext_char *lastChar = nullptr;
    bool ok = false;
    fz_try(m_ctx) {
        page = textPage(pageNumber);
        for (fz_stext_block *block = page->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
                if (!line->first_char)
                    continue;
                if (!firstChar)
                    firstChar = line->first_char;
                lastChar = line->last_char;
            }
        }
        if (firstChar && lastChar) {
            const QPointF origin = m_origins.at(pageNumber);
            const auto center = [&origin](const fz_quad &q) {
                return QPointF((q.ul.x + q.ur.x + q.ll.x + q.lr.x) / 4 - origin.x(),
                               (q.ul.y + q.ur.y + q.ll.y + q.lr.y) / 4 - origin.y());
            };
            first = center(firstChar->quad);
            last = center(lastChar->quad);
            ok = true;
        }
    }
    fz_catch(m_ctx) {
        return false;
    }
    return ok;
}

fz_stext_page *Document::textPage(int pageNumber) const
{
    if (fz_stext_page *cached = m_textPages.value(pageNumber, nullptr))
        return cached;
    fz_stext_page *page = fz_new_stext_page_from_page_number(
        m_ctx, m_doc, pageNumber, nullptr);
    m_textPages.insert(pageNumber, page);
    return page;
}

namespace {

// What kind of content a PDF structure element declares, or `inherited` when it
// only groups (document, section, paragraph, span...).
QString tagForStructure(fz_structure type, const QString &inherited)
{
    switch (type) {
    case FZ_STRUCTURE_ARTIFACT: return QStringLiteral("artifact");
    case FZ_STRUCTURE_FIGURE: return QStringLiteral("figure");
    case FZ_STRUCTURE_FORMULA: return QStringLiteral("formula");
    case FZ_STRUCTURE_TABLE: case FZ_STRUCTURE_TR: case FZ_STRUCTURE_TH: case FZ_STRUCTURE_TD:
    case FZ_STRUCTURE_THEAD: case FZ_STRUCTURE_TBODY: case FZ_STRUCTURE_TFOOT:
        return QStringLiteral("table");
    case FZ_STRUCTURE_CAPTION: return QStringLiteral("caption");
    case FZ_STRUCTURE_FENOTE: case FZ_STRUCTURE_NOTE: return QStringLiteral("footnote");
    case FZ_STRUCTURE_REFERENCE: case FZ_STRUCTURE_BIBENTRY: return QStringLiteral("reference");
    case FZ_STRUCTURE_TOC: case FZ_STRUCTURE_TOCI: case FZ_STRUCTURE_INDEX:
        return QStringLiteral("toc");
    case FZ_STRUCTURE_H: case FZ_STRUCTURE_H1: case FZ_STRUCTURE_H2: case FZ_STRUCTURE_H3:
    case FZ_STRUCTURE_H4: case FZ_STRUCTURE_H5: case FZ_STRUCTURE_H6: case FZ_STRUCTURE_TITLE:
        return QStringLiteral("heading");
    default:
        return inherited;
    }
}

bool isMathFont(const char *name)
{
    if (!name)
        return false;
    static const char *const markers[] = {
        "cmmi", "cmsy", "cmex", "msbm", "msam", "eufm", "rtxmi", "rtxsy", "txmi", "txsy",
        "mtsy", "mtmi", "mtextra", "stixmath", "cambriamath", "lmmath", "xitsmath",
        "pxmi", "pxsy", "mathematicalpi", "symbolmt", "mathit", "eusm", "lmmi", "lmsy"};
    const QByteArray lower = QByteArray(name).toLower();
    for (const char *m : markers)
        if (lower.contains(m))
            return true;
    return false;
}

// Collects a page's text lines (with the PDF's own structure tags and the
// extent of every character) and its pictures.
struct ReadCollector {
    fz_context *ctx;
    QPointF origin;
    QVector<ReadLine> *lines;
    QVector<QRectF> images;
    QVector<QRectF> grids;     // tables as MuPDF's own table finder sees them

    void walk(fz_stext_block *first, const QString &tag, bool tagged)
    {
        for (fz_stext_block *block = first; block; block = block->next) {
            if (block->type == FZ_STEXT_BLOCK_STRUCT) {
                fz_stext_struct *node = block->u.s.down;
                if (node)
                    walk(node->first_block, tagForStructure(node->standard, tag), true);
            } else if (block->type == FZ_STEXT_BLOCK_GRID) {
                grids.append(QRectF(block->bbox.x0 - origin.x(), block->bbox.y0 - origin.y(),
                                    block->bbox.x1 - block->bbox.x0, block->bbox.y1 - block->bbox.y0));
            } else if (block->type == FZ_STEXT_BLOCK_IMAGE) {
                images.append(QRectF(block->bbox.x0 - origin.x(), block->bbox.y0 - origin.y(),
                                     block->bbox.x1 - block->bbox.x0, block->bbox.y1 - block->bbox.y0));
            } else if (block->type == FZ_STEXT_BLOCK_TEXT) {
                for (fz_stext_line *line = block->u.t.first_line; line; line = line->next)
                    addLine(block, line, tag, tagged);
            }
        }
    }

    void addLine(fz_stext_block *block, fz_stext_line *line, const QString &tag, bool tagged)
    {
        // Rotated lines are watermarks and margin stamps.
        if (!line->first_char || std::abs(line->dir.y) > 0.1f || line->dir.x <= 0)
            return;
        float maxSize = 0;
        int boldChars = 0, mathChars = 0, chars = 0;
        for (fz_stext_char *ch = line->first_char; ch; ch = ch->next) {
            maxSize = std::max(maxSize, ch->size);
            ++chars;
            if (ch->flags & FZ_STEXT_BOLD)
                ++boldChars;
            if (ch->font && isMathFont(fz_font_name(ctx, ch->font)))
                ++mathChars;
        }
        // The text is built already normalized (ligatures expanded, whitespace
        // collapsed) with the extent of every character, so a position in the
        // text maps back to a place on the page.
        QString s;
        QVector<float> cx0, cx1;
        bool first = true;
        for (fz_stext_char *ch = line->first_char; ch; ch = ch->next) {
            // Drop superscript footnote markers and citation numbers.
            const bool sup = !first && ch->size < maxSize * 0.75f
                && (QChar::isDigit(uint(ch->c)) || ch->c == '*'
                    || ch->c == 0x2020 || ch->c == 0x2021 || ch->c == ',');
            first = false;
            if (sup)
                continue;
            const char32_t code = char32_t(ch->c);
            const QString piece = QString::fromUcs4(&code, 1).normalized(QString::NormalizationForm_KC);
            const float a = std::min(ch->quad.ul.x, ch->quad.ll.x) - float(origin.x());
            const float b = std::max(ch->quad.ur.x, ch->quad.lr.x) - float(origin.x());
            const float step = (b - a) / std::max<qsizetype>(1, piece.size());
            for (qsizetype k = 0; k < piece.size(); ++k) {
                if (piece[k].isSpace()) {
                    if (!s.isEmpty() && !s.endsWith(QLatin1Char(' '))) {
                        s += QLatin1Char(' ');
                        cx0.append(a + step * k);
                        cx1.append(a + step * (k + 1));
                    }
                } else {
                    s += piece[k];
                    cx0.append(a + step * k);
                    cx1.append(a + step * (k + 1));
                }
            }
        }
        while (s.endsWith(QLatin1Char(' '))) {
            s.chop(1);
            cx0.removeLast();
            cx1.removeLast();
        }
        ReadLine rl;
        rl.box = QRectF(line->bbox.x0 - origin.x(), line->bbox.y0 - origin.y(),
                        line->bbox.x1 - line->bbox.x0, line->bbox.y1 - line->bbox.y0);
        rl.size = maxSize;
        rl.bold = chars > 0 && boldChars * 2 > chars;
        rl.block = block->id;
        rl.text = s;
        rl.cx0 = cx0;
        rl.cx1 = cx1;
        rl.tag = tag;
        rl.tagged = tagged;
        rl.mathFraction = chars > 0 ? double(mathChars) / chars : 0.0;
        if (qEnvironmentVariableIsSet("GLANCE_READ_TAGS"))
            fprintf(stderr, "tag %-9s %s %s\n", rl.tag.toUtf8().constData(), tagged ? "T" : "-",
                    rl.text.left(60).toUtf8().constData());
        lines->push_back(rl);
    }

    // Structure elements and font changes split a row into several lines (an
    // inline variable, a subscript). Put pieces that share a row back together.
    void mergeSameRow()
    {
        QVector<ReadLine> out;
        for (ReadLine &l : *lines) {
            if (!out.isEmpty()) {
                ReadLine &p = out.last();
                const double overlap = std::min(p.box.bottom(), l.box.bottom()) - std::max(p.box.top(), l.box.top());
                const double gap = l.box.left() - p.box.right();
                const double reach = std::max(p.size, l.size);
                if (overlap > 0.6 * std::min(p.box.height(), l.box.height())
                    && gap > -2 && gap < reach * 1.2 && !p.text.isEmpty() && !l.text.isEmpty()) {
                    const bool space = gap > reach * 0.15;
                    if (space) {
                        p.text += QLatin1Char(' ');
                        p.cx0.append(float(p.box.right()));
                        p.cx1.append(float(l.box.left()));
                    }
                    const int pn = p.text.size();
                    p.text += l.text;
                    p.cx0 += l.cx0;
                    p.cx1 += l.cx1;
                    Q_UNUSED(pn);
                    const double total = double(p.text.size());
                    const double lw = double(l.text.size());
                    p.mathFraction = (p.mathFraction * (total - lw) + l.mathFraction * lw) / std::max(1.0, total);
                    if (l.text.size() > p.text.size() - l.text.size()) {
                        p.tag = l.tag;
                        p.tagged = l.tagged;
                    }
                    p.box = p.box.united(l.box);
                    p.size = std::max(p.size, l.size);
                    p.bold = p.bold && l.bold;
                    continue;
                }
            }
            out.append(l);
        }
        *lines = out;
    }

    // Only grids that look like tables: not a page-sized frame (slide backgrounds,
    // page borders) and holding several lines of text.
    void markGrids(QSizeF pageSize)
    {
        for (const QRectF &g : std::as_const(grids)) {
            if (g.width() * g.height() > 0.6 * pageSize.width() * pageSize.height())
                continue;
            const QRectF area = g.adjusted(-2, -2, 2, 2);
            int inside = 0;
            for (const ReadLine &l : *lines)
                if (area.contains(l.box.center()))
                    ++inside;
            if (inside < 4)
                continue;
            for (ReadLine &l : *lines)
                if (area.contains(l.box.center()))
                    l.inGrid = true;
        }
    }

    // Share of each line lying over a picture. Full-page images are scans or
    // backgrounds, not figures.
    void markPictures(QSizeF pageSize)
    {
        QVector<QRectF> figures;
        for (const QRectF &r : std::as_const(images))
            if (r.width() * r.height() < 0.7 * pageSize.width() * pageSize.height())
                figures.append(r);
        if (figures.isEmpty())
            return;
        for (ReadLine &l : *lines) {
            const double area = std::max(1.0, l.box.width() * l.box.height());
            double best = 0;
            for (const QRectF &r : std::as_const(figures)) {
                const QRectF i = l.box.intersected(r);
                if (!i.isEmpty())
                    best = std::max(best, i.width() * i.height() / area);
            }
            l.imageFraction = best;
        }
    }
};

} // namespace

QVariantList Document::readingParagraphs(const QVariantMap &options) const
{
    QVariantList result;
    if (!isOpen())
        return result;

    ReadOptions opts;
    const auto flag = [&](const char *key, bool fallback) {
        return options.contains(QLatin1String(key)) ? options.value(QLatin1String(key)).toBool()
                                                    : fallback;
    };
    opts.skipHeadersFooters = flag("skipHeadersFooters", opts.skipHeadersFooters);
    opts.skipFootnotes = flag("skipFootnotes", opts.skipFootnotes);
    opts.skipCaptions = flag("skipCaptions", opts.skipCaptions);
    opts.skipTables = flag("skipTables", opts.skipTables);
    opts.skipReferences = flag("skipReferences", opts.skipReferences);

    opts.skipMath = flag("skipMath", opts.skipMath);

    QVector<QVector<ReadLine>> pages(m_pageCount);
    QVector<QSizeF> sizes(m_pageCount);
    {
        QMutexLocker lock(&m_mutex);
        for (int p = 0; p < m_pageCount; ++p) {
            sizes[p] = m_sizes.at(p);
            const QPointF origin = m_origins.at(p);
            fz_stext_page *text = nullptr;
            fz_try(m_ctx) {
                fz_stext_options sopts{};
                sopts.flags = FZ_STEXT_COLLECT_STRUCTURE | FZ_STEXT_PRESERVE_IMAGES | FZ_STEXT_TABLE_HUNT;
                text = fz_new_stext_page_from_page_number(m_ctx, m_doc, p, &sopts);
                ReadCollector collector{m_ctx, origin, &pages[p], {}};
                collector.walk(text->first_block, QString(), false);
                collector.mergeSameRow();
                collector.markPictures(sizes[p]);
                collector.markGrids(sizes[p]);
            }
            fz_always(m_ctx) {
                fz_drop_stext_page(m_ctx, text);
            }
            fz_catch(m_ctx) {
                qWarning() << "glance: reading text failed on page" << p;
            }
        }
    }

    const QVector<ReadParagraph> built = buildReadingParagraphs(pages, sizes, opts, ReadModel::load());
    {
        QMutexLocker lock(&m_readMutex);
        m_readCache = built;
    }
    for (const ReadParagraph &para : built) {
        QVariantList featureList;
        for (double v : para.features)
            featureList.append(v);
        QVariantList boxes;
        for (const ReadBox &b : para.boxes)
            boxes.append(QVariantMap{{QStringLiteral("page"), b.page},
                                     {QStringLiteral("x"), b.rect.x()}, {QStringLiteral("y"), b.rect.y()},
                                     {QStringLiteral("w"), b.rect.width()}, {QStringLiteral("h"), b.rect.height()},
                                     {QStringLiteral("start"), b.start}, {QStringLiteral("end"), b.end}});
        result.append(QVariantMap{{QStringLiteral("page"), para.page},
                                  {QStringLiteral("text"), para.text},
                                  {QStringLiteral("kind"), para.kind},
                                  {QStringLiteral("skipped"), para.skipped},
                                  {QStringLiteral("boxes"), boxes},
                                  {QStringLiteral("features"), featureList}});
    }
    return result;
}

// Where a character offset of a paragraph (as returned by readingParagraphs,
// addressed by its position in that list) sits on the page: the word around it
// and the line it is on, in page points.
QVariantMap Document::readingWordAt(int paragraph, int offset) const
{
    QMutexLocker lock(&m_readMutex);
    if (paragraph < 0 || paragraph >= m_readCache.size())
        return {};
    const ReadParagraph &para = m_readCache.at(paragraph);
    const QString &text = para.text;
    if (text.isEmpty() || para.boxes.isEmpty())
        return {};
    offset = std::clamp(offset, 0, int(text.size()) - 1);
    while (offset < text.size() - 1 && text[offset].isSpace())
        ++offset;
    int ws = offset, we = offset;
    while (ws > 0 && !text[ws - 1].isSpace())
        --ws;
    while (we < text.size() && !text[we].isSpace())
        ++we;

    int bi = 0;
    for (int i = 0; i < para.boxes.size(); ++i)
        if (para.boxes[i].start <= offset)
            bi = i;
    const ReadBox &b = para.boxes[bi];
    const int n = std::min<int>(b.cx0.size(), b.cx1.size());
    if (n == 0)
        return {};
    const int from = std::clamp(ws - b.start, 0, n - 1);
    const int to = std::clamp(we - b.start - 1, from, n - 1);
    const double x0 = b.cx0[from];
    const double x1 = b.cx1[to];
    return QVariantMap{{QStringLiteral("page"), b.page},
                       {QStringLiteral("x"), x0}, {QStringLiteral("y"), b.rect.y()},
                       {QStringLiteral("w"), std::max(1.0, x1 - x0)}, {QStringLiteral("h"), b.rect.height()},
                       {QStringLiteral("wordStart"), ws}, {QStringLiteral("wordEnd"), we},
                       {QStringLiteral("lineX"), b.rect.x()}, {QStringLiteral("lineW"), b.rect.width()},
                       {QStringLiteral("line"), bi}};
}

void Document::teachReading(const QVariantList &features, bool junk) const
{
    QVector<double> f;
    for (const QVariant &v : features)
        f.append(v.toDouble());
    if (f.size() != ReadModel::kFeatures)
        return;
    ReadModel model = ReadModel::load();
    model.learn(f, junk);
    model.save();
}

void Document::resetReadingModel() const
{
    ReadModel model = ReadModel::load();
    model.reset();
    model.save();
}

bool Document::copyTextToClipboard(const QString &text) const
{
    if (text.isEmpty())
        return false;
    QGuiApplication::clipboard()->setText(text);
    return true;
}

bool Document::copyPageToClipboard(int page) const
{
    const QImage img = renderPage(page, 2.0);
    if (img.isNull())
        return false;
    QGuiApplication::clipboard()->setImage(img);
    return true;
}
