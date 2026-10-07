#include "layoutmanager.h"

#include "document.h"
#include "reader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <QThreadPool>
#include <algorithm>

namespace {

constexpr qreal kScale = 1.5;      // pixels per point of the image the model looks at
constexpr int kWorkers = 2;
constexpr int kThreadsPerWorker = 6;
constexpr const char *kModelName = "pp_doc_layoutv2.onnx";

QString workerScript()
{
    const QString installed = QCoreApplication::applicationDirPath()
        + QStringLiteral("/glance-layout-worker");
    return QFileInfo::exists(installed) ? installed : QStringLiteral(LAYOUT_WORKER_SOURCE_S);
}

QString modelPath() { return Reader::dataDir() + QLatin1Char('/') + QLatin1String(kModelName); }

QVector<LayoutRegion> regionsFromJson(const QJsonArray &array)
{
    QVector<LayoutRegion> out;
    for (const QJsonValue &v : array) {
        const QJsonObject o = v.toObject();
        const QJsonArray box = o.value(QStringLiteral("box")).toArray();
        if (box.size() != 4)
            continue;
        LayoutRegion r;
        r.label = o.value(QStringLiteral("label")).toString();
        r.score = o.value(QStringLiteral("score")).toDouble();
        r.rect = QRectF(box[0].toDouble() / kScale, box[1].toDouble() / kScale,
                        (box[2].toDouble() - box[0].toDouble()) / kScale,
                        (box[3].toDouble() - box[1].toDouble()) / kScale);
        out.append(r);
    }
    return out;
}

} // namespace

LayoutManager::LayoutManager(Document *doc, QObject *parent)
    : QObject(parent), m_doc(doc)
{
    connect(m_doc, &Document::pageCountChanged, this, [this] { reset(); });
    reset();
}

LayoutManager::~LayoutManager()
{
    stop();
}

bool LayoutManager::available() const
{
    const QString dir = Reader::dataDir();
    if (!QFileInfo::exists(dir + QStringLiteral("/runtime/bin/python"))
        || !QFileInfo::exists(modelPath()) || !QFileInfo::exists(workerScript()))
        return false;
    const QDir lib(dir + QStringLiteral("/runtime/lib"));
    for (const QString &py : lib.entryList({QStringLiteral("python3*")}, QDir::Dirs))
        if (QFileInfo::exists(lib.filePath(py + QStringLiteral("/site-packages/rapid_layout"))))
            return true;
    return false;
}

void LayoutManager::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    if (!on)
        stop();
}

int LayoutManager::activeJobs() const
{
    int n = 0;
    for (const Worker *w : m_workers)
        if (w->page >= 0)
            ++n;
    return n;
}

QString LayoutManager::cacheFile(int page) const
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/layout/") + m_cacheKey + QLatin1Char('/') + QString::number(page)
        + QStringLiteral(".json");
}

void LayoutManager::reset()
{
    ++m_generation;
    m_queue.clear();
    m_wantFirst = -1;
    m_wantCount = 0;
    for (Worker *w : std::as_const(m_workers))
        w->page = -1;
    const QFileInfo info(m_doc->filePath());
    // Page layout depends on the file's contents and on the model that read it.
    m_cacheKey = QString::fromLatin1(QCryptographicHash::hash(
        QStringLiteral("%1|%2|%3|v2").arg(info.canonicalFilePath()).arg(info.size())
            .arg(info.lastModified().toMSecsSinceEpoch()).toUtf8(),
        QCryptographicHash::Sha1).toHex());
    if (m_wasBusy) {
        m_wasBusy = false;
        emit busyChanged();
    }
}

bool LayoutManager::isReady(int first, int count) const
{
    const int last = std::min(m_doc->pageCount(), first + count);
    for (int p = std::max(0, first); p < last; ++p)
        if (!m_doc->hasPageLayout(p))
            return false;
    return true;
}

void LayoutManager::enqueueFront(int page)
{
    if (page < 0 || page >= m_doc->pageCount() || m_doc->hasPageLayout(page))
        return;
    m_queue.removeAll(page);
    m_queue.prepend(page);
}

void LayoutManager::enqueueBack(int page)
{
    if (page < 0 || page >= m_doc->pageCount() || m_doc->hasPageLayout(page) || m_queue.contains(page))
        return;
    for (const Worker *w : m_workers)
        if (w->page == page)
            return;
    m_queue.append(page);
}

bool LayoutManager::ensure(int first, int count)
{
    if (!m_enabled || !available() || m_doc->pageCount() == 0)
        return true;     // nothing will ever arrive: go on with the heuristics alone
    first = std::clamp(first, 0, m_doc->pageCount() - 1);
    count = std::max(1, std::min(count, m_doc->pageCount() - first));
    if (isReady(first, count))
        return true;
    m_wantFirst = first;
    m_wantCount = count;
    for (int p = first + count - 1; p >= first; --p)
        enqueueFront(p);
    pump();
    return isReady(first, count);
}

void LayoutManager::analyzeFrom(int page)
{
    if (!m_enabled || !available())
        return;
    const int n = m_doc->pageCount();
    for (int p = std::max(0, page); p < n; ++p)
        enqueueBack(p);
    for (int p = 0; p < std::min(page, n); ++p)
        enqueueBack(p);
    pump();
}

void LayoutManager::stop()
{
    ++m_generation;
    m_queue.clear();
    m_wantFirst = -1;
    for (Worker *w : std::as_const(m_workers)) {
        if (w->process) {
            w->process->disconnect(this);
            w->process->kill();
            w->process->waitForFinished(500);
            w->process->deleteLater();
        }
        delete w;
    }
    m_workers.clear();
    if (m_wasBusy) {
        m_wasBusy = false;
        emit busyChanged();
    }
}

bool LayoutManager::loadCached(int page)
{
    QFile f(cacheFile(page));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray())
        return false;
    QVector<LayoutRegion> regions;
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        LayoutRegion r;
        r.label = o.value(QStringLiteral("label")).toString();
        r.score = o.value(QStringLiteral("score")).toDouble();
        r.rect = QRectF(o.value(QStringLiteral("x")).toDouble(), o.value(QStringLiteral("y")).toDouble(),
                        o.value(QStringLiteral("w")).toDouble(), o.value(QStringLiteral("h")).toDouble());
        regions.append(r);
    }
    m_doc->setPageLayout(page, regions);
    return true;
}

void LayoutManager::saveCached(int page, const QByteArray &json) const
{
    QDir().mkpath(QFileInfo(cacheFile(page)).absolutePath());
    QFile f(cacheFile(page));
    if (f.open(QIODevice::WriteOnly))
        f.write(json);
}

void LayoutManager::startWorker()
{
    auto *w = new Worker;
    w->process = new QProcess(this);
    w->process->setStandardErrorFile(QProcess::nullDevice());
    connect(w->process, &QProcess::readyReadStandardOutput, this, [this, w] { handleOutput(*w); });
    connect(w->process, &QProcess::finished, this, [this, w] {
        // A worker that dies takes its page with it; the page is simply not analysed.
        w->ready = false;
        w->page = -1;
        if (!m_workers.contains(w))
            return;
        m_workers.removeAll(w);
        w->process->deleteLater();
        delete w;
        if (m_wasBusy && !busy()) {
            m_wasBusy = false;
            emit busyChanged();
        }
    });
    // Low priority: this work must never get in the way of the voice or the window.
    const QString python = Reader::dataDir() + QStringLiteral("/runtime/bin/python");
    const QStringList args{workerScript(), modelPath(), QString::number(kThreadsPerWorker)};
    const QString nice = QStandardPaths::findExecutable(QStringLiteral("nice"));
    if (nice.isEmpty())
        w->process->start(python, args);
    else
        w->process->start(nice, QStringList{QStringLiteral("-n"), QStringLiteral("10"), python} + args);
    m_workers.append(w);
}

void LayoutManager::pump()
{
    if (!m_enabled)
        return;
    // Anything already on disk needs no worker at all.
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (loadCached(*it)) {
            const int page = *it;
            it = m_queue.erase(it);
            emit pageAnalyzed(page);
        } else {
            ++it;
        }
    }
    checkRange();
    if (m_queue.isEmpty()) {
        if (m_wasBusy && !busy()) {
            m_wasBusy = false;
            emit busyChanged();
        }
        return;
    }
    if (!m_wasBusy) {
        m_wasBusy = true;
        emit busyChanged();
    }
    while (m_workers.size() < kWorkers && m_workers.size() < m_queue.size())
        startWorker();
    for (Worker *w : std::as_const(m_workers)) {
        if (m_queue.isEmpty())
            break;
        if (w->ready && w->page < 0)
            begin(*w, m_queue.takeFirst());
    }
}

void LayoutManager::begin(Worker &worker, int page)
{
    worker.page = page;
    worker.generation = m_generation;
    worker.imagePath = m_tmp.path() + QStringLiteral("/p%1_%2.png").arg(m_generation).arg(page);
    const int generation = m_generation;
    const QString path = worker.imagePath;
    QPointer<LayoutManager> self(this);
    Document *doc = m_doc;
    QThreadPool::globalInstance()->start([self, doc, generation, page, path] {
        const QImage image = doc->renderPage(page, kScale);
        const bool ok = !image.isNull() && image.save(path, "PNG");
        if (self)
            QMetaObject::invokeMethod(self.data(), [self, generation, page, path, ok] {
                if (self)
                    self->rendered(generation, page, path, ok);
            }, Qt::QueuedConnection);
    });
}

void LayoutManager::rendered(int generation, int page, const QString &path, bool ok)
{
    Worker *worker = nullptr;
    for (Worker *w : std::as_const(m_workers))
        if (w->page == page && w->generation == generation)
            worker = w;
    if (!worker || generation != m_generation) {
        QFile::remove(path);
        return;
    }
    if (!ok) {
        worker->page = -1;
        pump();
        return;
    }
    const QJsonObject req{{QStringLiteral("id"), page}, {QStringLiteral("path"), path}};
    worker->process->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
}

void LayoutManager::handleOutput(Worker &worker)
{
    worker.buffer += worker.process->readAllStandardOutput();
    for (;;) {
        const int nl = worker.buffer.indexOf('\n');
        if (nl < 0)
            return;
        const QJsonObject obj = QJsonDocument::fromJson(worker.buffer.left(nl)).object();
        worker.buffer.remove(0, nl + 1);
        if (obj.value(QStringLiteral("ready")).toBool()) {
            worker.ready = true;
            pump();
            continue;
        }
        const int page = obj.value(QStringLiteral("id")).toInt(-1);
        QFile::remove(worker.imagePath);
        const bool current = worker.generation == m_generation && worker.page == page;
        worker.page = -1;
        if (current && obj.contains(QStringLiteral("dets"))) {
            const QJsonArray dets = obj.value(QStringLiteral("dets")).toArray();
            const QVector<LayoutRegion> regions = regionsFromJson(dets);
            m_doc->setPageLayout(page, regions);
            // Cache in page points, ready to use.
            QJsonArray cached;
            for (const LayoutRegion &r : regions)
                cached.append(QJsonObject{{QStringLiteral("label"), r.label}, {QStringLiteral("score"), r.score},
                                          {QStringLiteral("x"), r.rect.x()}, {QStringLiteral("y"), r.rect.y()},
                                          {QStringLiteral("w"), r.rect.width()}, {QStringLiteral("h"), r.rect.height()}});
            saveCached(page, QJsonDocument(cached).toJson(QJsonDocument::Compact));
            emit pageAnalyzed(page);
        }
        pump();
    }
}

void LayoutManager::checkRange()
{
    if (m_wantFirst >= 0 && isReady(m_wantFirst, m_wantCount)) {
        m_wantFirst = -1;
        emit rangeReady();
    }
}
