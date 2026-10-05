#include "document.h"

#include <QFileInfo>
#include <QThreadPool>
#include <QGuiApplication>
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
    if (!path.isEmpty())
        load(path);
}

Document::~Document()
{
    reset();
}

void Document::reset()
{
    m_labels.clear();
    m_labelsBuilt = false;
    cancelSearch();
    m_textPool.waitForDone();
    m_renderPool.waitForDone();
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

bool Document::load(const QString &path)
{
    m_path = path;

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

    fz_try(m_ctx)
        m_doc = fz_open_document(m_ctx, path.toUtf8().constData());
    fz_catch(m_ctx) {
        qWarning() << "glance: cannot open" << path;
        fz_drop_context(m_ctx);
        m_ctx = nullptr;
        m_path.clear();
        return false;
    }

    computeSizes();
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
    QMutexLocker lock(&m_mutex);
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
    fz_try(m_ctx) {
        page = textPage(pageNumber);
        for (fz_stext_block *block = page->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
                if (line->first_char)
                    return true;
            }
        }
    }
    fz_catch(m_ctx) {
        return false;
    }
    return false;
}

void Document::ensureLabels() const
{
    QMutexLocker lock(&m_mutex);
    if (m_labelsBuilt)
        return;
    m_labelsBuilt = true;
    m_labels.reserve(m_pageCount);
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
        m_labels.append(label);
    }
}

QString Document::pageLabel(int page) const
{
    if (!isOpen() || page < 0 || page >= m_pageCount)
        return QString();
    ensureLabels();
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
    QVariantMap result{{QStringLiteral("text"), QString()},
                       {QStringLiteral("boxes"), QVariantList()}};
    if (!isOpen() || pageNumber < 0 || pageNumber >= m_pageCount)
        return result;

    QMutexLocker lock(&m_mutex);
    char *text = nullptr;
    fz_try(m_ctx) {
        const QPointF origin = m_origins.at(pageNumber);
        const QSizeF size = m_sizes.at(pageNumber);
        const fz_rect bounds = fz_make_rect(
            origin.x(), origin.y(), origin.x() + size.width(), origin.y() + size.height());
        fz_stext_page *structuredText = textPage(pageNumber);
        fz_point a = fz_make_point(point.x() + bounds.x0, point.y() + bounds.y0);
        fz_point b = a;
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
        if (!firstChar || !lastChar)
            return false;
        const QPointF origin = m_origins.at(pageNumber);
        const auto center = [&origin](const fz_quad &q) {
            return QPointF((q.ul.x + q.ur.x + q.ll.x + q.lr.x) / 4 - origin.x(),
                           (q.ul.y + q.ur.y + q.ll.y + q.lr.y) / 4 - origin.y());
        };
        first = center(firstChar->quad);
        last = center(lastChar->quad);
    }
    fz_catch(m_ctx) {
        return false;
    }
    return true;
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
