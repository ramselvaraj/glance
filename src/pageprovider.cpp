#include "pageprovider.h"

#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QThreadPool>
#include <QDebug>

#include <algorithm>
#include <cmath>

namespace {


struct Request {
    bool valid = false;
    int page = -1;
    qreal scale = 1.0;
};

// p<page>_<exp>_<dpr> | t<page>_<dpr>
Request parseRequest(const QString &id, const Document *doc)
{
    Request r;
    const QStringList parts = id.split('_');
    if (parts.isEmpty())
        return r;

    const QString head = parts.at(0);
    if (head.size() < 2)
        return r;

    bool okPage = false;
    const int page = head.mid(1).toInt(&okPage);
    if (!okPage)
        return r;

    r.page = page;

    if (head.startsWith('p') && parts.size() == 3) {
        bool okExp = false;
        const int exp = parts.at(1).toInt(&okExp);
        const qreal dpr = parts.at(2).toDouble();
        if (!okExp || dpr <= 0)
            return r;
        // cap final texture scale so no dimension exceeds 8192 px
        const QSizeF pts = doc->pageSizePt(page);
        const qreal cap = 8192.0 / std::max(1.0, std::max(pts.width(), pts.height()));
        r.scale = std::clamp(std::pow(1.25, exp) * dpr, 0.01, cap);
        r.valid = true;
    } else if (head.startsWith('t') && parts.size() == 2) {
        const qreal dpr = parts.at(1).toDouble();
        if (dpr <= 0)
            return r;
        const QSizeF pts = doc->pageSizePt(page);
        r.scale = pts.width() > 0 ? (140.0 * dpr) / pts.width() : dpr;
        r.valid = true;
    }
    return r;
}

// Runs on a pool thread (auto-deleted by the pool); the response lives on the
// GUI thread and receives results via a queued signal.
class RenderRunnable : public QObject, public QRunnable {
    Q_OBJECT
public:
    RenderRunnable(const Document *doc, int page, qreal scale)
        : m_doc(doc), m_page(page), m_scale(scale)
    {
    }

    void run() override
    {
        emit done(m_doc->renderPage(m_page, m_scale));
    }

signals:
    void done(const QImage &image);

private:
    const Document *m_doc;
    int m_page;
    qreal m_scale;
};

// Lives on the GUI thread for as long as the engine needs it; engine deletes.
class PageResponse : public QQuickImageResponse {
public:
    PageResponse(const Document *doc, int page, qreal scale, QThreadPool *pool)
    {
        auto *runnable = new RenderRunnable(doc, page, scale);
        connect(runnable, &RenderRunnable::done, this, [this](const QImage &img) {
            m_image = img;
            emit finished();
        });
        pool->start(runnable);
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return m_image.isNull()
                   ? nullptr
                   : QQuickTextureFactory::textureFactoryForImage(m_image);
    }

private:
    QImage m_image;
};

} // namespace

QQuickImageResponse *PageProvider::requestImageResponse(const QString &id,
                                                        const QSize &requestedSize)
{
    Q_UNUSED(requestedSize);

    const Request r = parseRequest(id, m_doc);
    if (!r.valid || r.page < 0 || r.page >= m_doc->pageCount())
        return new PageResponse(m_doc, -1, 0.0, m_doc->renderPool());

    return new PageResponse(m_doc, r.page, r.scale, m_doc->renderPool());
}

#include "pageprovider.moc"
