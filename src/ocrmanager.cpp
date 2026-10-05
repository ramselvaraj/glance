#include "ocrmanager.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QDir>
#include <QSaveFile>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QProcessEnvironment>
#include <limits>
#include <QTextStream>

OcrManager::OcrManager(QObject *parent)
    : QObject(parent)
{
    connect(&m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
        if (m_cancelled || m_stage == Stage::Idle)
            return;
        if (status != QProcess::NormalExit || exitCode != 0) {
            finishWithError(QString::fromUtf8(m_process.readAllStandardError()).trimmed());
            return;
        }
        if (m_stage == Stage::Rendering) {
            setProgress(45);
            startRecognition();
        } else {
            setProgress(95);
            parseTsv();
        }
    });
    const QString applicationDir = QCoreApplication::applicationDirPath();
    const QString configuredPython = QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("GLANCE_RAPIDOCR_PYTHON"));
    const QString rapidPython = configuredPython.isEmpty()
        ? applicationDir + QStringLiteral("/ocr-runtime/bin/python")
        : configuredPython;
    QString rapidWorker = applicationDir + QStringLiteral("/glance-rapidocr-worker");
    if (!QFileInfo::exists(rapidWorker))
        rapidWorker = QStringLiteral(RAPIDOCR_WORKER_SOURCE_S);
    if (QFileInfo::exists(rapidPython) && QFileInfo::exists(rapidWorker)) {
        m_worker.setProgram(rapidPython);
        m_worker.setArguments({rapidWorker});
        m_workerIsRapid = true;
    } else {
        m_worker.setProgram(applicationDir + QStringLiteral("/glance-ocr-worker"));
    }
    m_worker.setStandardErrorFile(QProcess::nullDevice());
    connectWorker();
    m_workerAvailable = QFileInfo::exists(m_worker.program())
        && (!m_workerIsRapid || QFileInfo::exists(rapidWorker));
}

void OcrManager::connectWorker()
{
    connect(&m_worker, &QProcess::readyReadStandardOutput,
            this, &OcrManager::handleWorkerOutput);
    connect(&m_worker, &QProcess::finished, this, [this] {
        m_workerAvailable = false;
        if (!m_workerRequestActive)
            return;
        m_workerRequestActive = false;
        m_workerFailed = true;
        if (m_isPdf)
            fallbackToTesseract();
        else
            finishWithError(QStringLiteral("RapidOCR worker stopped"));
    });
}

OcrManager::~OcrManager()
{
    m_worker.disconnect();
    m_worker.terminate();
    if (!m_worker.waitForFinished(500)) {
        m_worker.kill();
        m_worker.waitForFinished(500);
    }
}

void OcrManager::prewarm()
{
    if (m_workerAvailable && m_worker.state() == QProcess::NotRunning)
        m_worker.start();
}

QString OcrManager::pageCacheKey(const QString &backend, const QString &tag) const
{
    const QFileInfo info(m_path);
    return QStringLiteral("v8|%1|%2|%3|%4|%5|%6|%7|eng|200")
        .arg(backend)
        .arg(info.canonicalFilePath())
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch())
        .arg(m_page)
        .arg(QString::number(m_pageSize.width(), 'f', 2)
             + QLatin1Char('x') + QString::number(m_pageSize.height(), 'f', 2))
        .arg(tag);
}

void OcrManager::recognize(const QString &path, int page, QSizeF pageSize,
                           int generation, const QVariantList &regions)
{
    cancel();
    prewarm();
    begin(path, page, pageSize, generation);
    m_isRegion = false;
    m_workerFailed = false;
    m_fallbackPass = false;
    m_region = QRectF();
    m_accum.clear();
    m_jobs.clear();
    m_jobIndex = 0;
    m_rapidMode = m_isPdf && m_workerAvailable && m_workerIsRapid;

    if (m_rapidMode) {
        const QRectF pageRect(QPointF(0, 0), pageSize);
        QStringList tag;
        for (const QVariant &v : regions) {
            const QVariantMap r = v.toMap();
            const QRectF rect = QRectF(r.value(QStringLiteral("x")).toDouble(),
                                       r.value(QStringLiteral("y")).toDouble(),
                                       r.value(QStringLiteral("w")).toDouble(),
                                       r.value(QStringLiteral("h")).toDouble())
                                    .intersected(pageRect);
            if (rect.width() < 4 || rect.height() < 4)
                continue;
            m_jobs.append(rect);
            tag << QStringLiteral("%1,%2,%3,%4").arg(rect.x(), 0, 'f', 0).arg(rect.y(), 0, 'f', 0)
                       .arg(rect.width(), 0, 'f', 0).arg(rect.height(), 0, 'f', 0);
        }
        if (m_jobs.isEmpty()) {
            m_jobs.append(pageRect);
            tag << QStringLiteral("page");
        }
        m_regionsTag = tag.join(QLatin1Char(';'));
        m_cacheKey = pageCacheKey(QStringLiteral("rapidocr-3.9.2"), m_regionsTag);
    } else {
        m_regionsTag.clear();
        m_cacheKey = pageCacheKey(QStringLiteral("tesseract-5"), QString());
    }

    Result result;
    const auto cached = m_cache.constFind(m_cacheKey);
    if (m_cacheEnabled
            && (cached != m_cache.cend() || loadCached(m_cacheKey, result))) {
        if (cached != m_cache.cend())
            result = cached.value();
        else
            m_cache.insert(m_cacheKey, result);
        QMetaObject::invokeMethod(this, [this, result, generation, page] {
            emit finished(generation, page, result.text, result.words, QString());
        }, Qt::QueuedConnection);
        return;
    }
    setProgress(5);
    setRunning(true);

    QFile::remove(m_tempDir.filePath(QStringLiteral("page.ppm")));
    QFile::remove(m_tempDir.filePath(QStringLiteral("ocr.tsv")));
    QFile::remove(m_tempDir.filePath(QStringLiteral("ocr.box")));

    if (!m_isPdf) {
        setProgress(45);
        startRecognition();
        return;
    }
    if (m_rapidMode)
        startNextJob();
    else
        startPageRender();
}

// Whole page at 200 dpi for Tesseract.
void OcrManager::startPageRender()
{
    m_stage = Stage::Rendering;
    m_scaleX = m_scaleY = 72.0 / 200.0;
    const QString prefix = m_tempDir.filePath(QStringLiteral("page"));
    m_process.setProgram(QStringLiteral("nice"));
    m_process.setArguments({QStringLiteral("-n"), QStringLiteral("10"),
                            QStringLiteral("pdftoppm"), QStringLiteral("-f"),
                            QString::number(m_page + 1), QStringLiteral("-l"),
                            QString::number(m_page + 1), QStringLiteral("-r"),
                            QStringLiteral("200"),
                            QStringLiteral("-singlefile"), m_path, prefix});
    m_process.start();
}

// RapidOCR: render the next crop at 200 dpi, then send it to the worker.
void OcrManager::startNextJob()
{
    const QRectF rect = m_jobs.at(m_jobIndex);
    const qreal pxPerPt = 200.0 / 72.0;
    const int px = qFloor(rect.x() * pxPerPt);
    const int py = qFloor(rect.y() * pxPerPt);
    const int pw = qMax(1, qCeil(rect.right() * pxPerPt) - px);
    const int ph = qMax(1, qCeil(rect.bottom() * pxPerPt) - py);
    m_jobOffsetX = px / pxPerPt;
    m_jobOffsetY = py / pxPerPt;
    m_scaleX = m_scaleY = 72.0 / 200.0;
    setProgress(5 + 40 * m_jobIndex / int(m_jobs.size()));
    m_stage = Stage::Rendering;
    QFile::remove(m_tempDir.filePath(QStringLiteral("page.ppm")));
    m_process.setProgram(QStringLiteral("nice"));
    m_process.setArguments({QStringLiteral("-n"), QStringLiteral("10"),
                            QStringLiteral("pdftoppm"), QStringLiteral("-f"),
                            QString::number(m_page + 1), QStringLiteral("-l"),
                            QString::number(m_page + 1), QStringLiteral("-r"),
                            QStringLiteral("200"),
                            QStringLiteral("-x"), QString::number(px),
                            QStringLiteral("-y"), QString::number(py),
                            QStringLiteral("-W"), QString::number(pw),
                            QStringLiteral("-H"), QString::number(ph),
                            QStringLiteral("-singlefile"), m_path,
                            m_tempDir.filePath(QStringLiteral("page"))});
    m_process.start();
}

// RapidOCR is unavailable or failed mid-page: redo the whole page with Tesseract.
void OcrManager::fallbackToTesseract()
{
    if (m_cancelled)
        return;
    const bool wasRapid = m_rapidMode;
    m_rapidMode = false;
    m_workerFailed = true;
    m_accum.clear();
    if (!wasRapid) {
        startTesseract();   // page.ppm already holds the full page
        return;
    }
    m_cacheKey = pageCacheKey(QStringLiteral("tesseract-5"), QString());
    startPageRender();
}

void OcrManager::begin(const QString &path, int page, QSizeF pageSize, int generation)
{
    m_cancelled = false;
    m_path = path;
    m_page = page;
    m_pageSize = pageSize;
    m_generation = generation;
    m_isPdf = QFileInfo(path).suffix().compare(QStringLiteral("pdf"),
                                               Qt::CaseInsensitive) == 0;
    m_scaleX = 72.0 / 200.0;
    m_scaleY = 72.0 / 200.0;
    if (!m_isPdf) {
        const QSize pixels = QImageReader(path).size();
        if (!pixels.isEmpty()) {
            m_scaleX = pageSize.width() / pixels.width();
            m_scaleY = pageSize.height() / pixels.height();
        }
    }
}

void OcrManager::recognizeRegion(const QString &path, int page, QSizeF pageSize,
                                 QRectF region, int generation)
{
    cancel();
    begin(path, page, pageSize, generation);
    m_isRegion = true;
    m_workerFailed = false;
    m_fallbackPass = false;
    m_region = region.intersected(QRectF(QPointF(0, 0), pageSize));
    if (m_region.isEmpty()) {
        emit regionFinished(generation, page, m_region, {}, QStringLiteral("Empty OCR region"));
        return;
    }
    const QFileInfo info(path);
    const QString backend = !m_isPdf && m_workerAvailable && m_workerIsRapid
        ? QStringLiteral("rapidocr-3.9.2") : QStringLiteral("tesseract-5");
    m_cacheKey = QStringLiteral("region-v4|%1|%2|%3|%4|%5|%6,%7,%8,%9|eng|6x")
        .arg(backend)
        .arg(info.canonicalFilePath())
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch())
        .arg(page)
        .arg(QString::number(m_region.x(), 'f', 1))
        .arg(QString::number(m_region.y(), 'f', 1))
        .arg(QString::number(m_region.width(), 'f', 1))
        .arg(QString::number(m_region.height(), 'f', 1));
    Result cached;
    const auto memory = m_cache.constFind(m_cacheKey);
    if (m_cacheEnabled
            && (memory != m_cache.cend() || loadCached(m_cacheKey, cached))) {
        if (memory != m_cache.cend())
            cached = memory.value();
        else
            m_cache.insert(m_cacheKey, cached);
        const QRectF cachedRegion = m_region;
        QMetaObject::invokeMethod(this,
            [this, cached, generation, page, cachedRegion] {
                emit regionFinished(generation, page, cachedRegion,
                                    cached.words, QString());
            }, Qt::QueuedConnection);
        return;
    }
    m_scaleX = 72.0 / 400.0;
    m_scaleY = 72.0 / 400.0;
    QFile::remove(m_tempDir.filePath(QStringLiteral("page.ppm")));
    QFile::remove(m_tempDir.filePath(QStringLiteral("ocr.tsv")));
    QFile::remove(m_tempDir.filePath(QStringLiteral("ocr.box")));
    if (!m_isPdf) {
        QImageReader reader(path);
        const QSize imageSize = reader.size();
        const QRect pixelRegion(qRound(m_region.x() * imageSize.width() / pageSize.width()),
                                qRound(m_region.y() * imageSize.height() / pageSize.height()),
                                qRound(m_region.width() * imageSize.width() / pageSize.width()),
                                qRound(m_region.height() * imageSize.height() / pageSize.height()));
        reader.setClipRect(pixelRegion);
        QImage crop = reader.read().scaled(pixelRegion.size() * 6,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation)
                                  .convertToFormat(QImage::Format_Grayscale8);
        if (!crop.isNull()) {
            int histogram[256] = {};
            for (int y = 0; y < crop.height(); ++y) {
                const uchar *line = crop.constScanLine(y);
                for (int x = 0; x < crop.width(); ++x)
                    ++histogram[line[x]];
            }
            const int pixels = crop.width() * crop.height();
            const int lowTarget = pixels * 2 / 100;
            const int highTarget = pixels * 98 / 100;
            int count = 0;
            int low = 0;
            int high = 255;
            for (; low < 255 && count + histogram[low] < lowTarget; ++low)
                count += histogram[low];
            count = 0;
            for (int value = 0; value < 256; ++value) {
                count += histogram[value];
                if (count >= highTarget) {
                    high = value;
                    break;
                }
            }
            if (high > low) {
                for (int y = 0; y < crop.height(); ++y) {
                    uchar *line = crop.scanLine(y);
                    for (int x = 0; x < crop.width(); ++x)
                        line[x] = qBound(0, (int(line[x]) - low) * 255 / (high - low), 255);
                }
            }
        }
        if (crop.isNull() || !crop.save(m_tempDir.filePath(QStringLiteral("page.ppm")))) {
            finishWithError(QStringLiteral("Could not prepare OCR region"));
            return;
        }
        m_scaleX = m_region.width() / crop.width();
        m_scaleY = m_region.height() / crop.height();
        setRunning(true);
        setProgress(45);
        startRecognition();
        return;
    }
    setRunning(true);
    setProgress(5);
    m_stage = Stage::Rendering;
    const qreal pixelsPerPoint = 400.0 / 72.0;
    m_process.setProgram(QStringLiteral("nice"));
    m_process.setArguments({QStringLiteral("-n"), QStringLiteral("0"),
                            QStringLiteral("pdftoppm"), QStringLiteral("-f"),
                            QString::number(page + 1), QStringLiteral("-l"),
                            QString::number(page + 1), QStringLiteral("-r"),
                            QStringLiteral("400"), QStringLiteral("-x"),
                            QString::number(qRound(m_region.x() * pixelsPerPoint)),
                            QStringLiteral("-y"),
                            QString::number(qRound(m_region.y() * pixelsPerPoint)),
                            QStringLiteral("-W"),
                            QString::number(qRound(m_region.width() * pixelsPerPoint)),
                            QStringLiteral("-H"),
                            QString::number(qRound(m_region.height() * pixelsPerPoint)),
                            QStringLiteral("-singlefile"),
                            path, m_tempDir.filePath(QStringLiteral("page"))});
    m_process.start();
}

void OcrManager::cancel()
{
    m_cancelled = true;
    m_stage = Stage::Idle;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
    if (m_workerRequestActive && m_worker.state() != QProcess::NotRunning) {
        m_workerRequestActive = false;
        m_worker.disconnect();
        m_worker.kill();
        m_worker.waitForFinished(1000);
        connectWorker();
        m_workerBuffer.clear();
        m_workerAvailable = QFileInfo::exists(m_worker.program());
    }
    setRunning(false);
    setProgress(0);
    m_workerRequestActive = false;
}

void OcrManager::setRunning(bool running)
{
    if (m_running == running)
        return;
    m_running = running;
    emit runningChanged();
}

void OcrManager::setProgress(int progress)
{
    if (m_progress == progress)
        return;
    m_progress = progress;
    emit progressChanged();
}

void OcrManager::startRecognition()
{
    if (m_rapidMode) {
        if (!m_workerFailed && !m_workerRequestActive && startWorkerRequest())
            return;
        fallbackToTesseract();
        return;
    }
    if (m_isPdf) {
        startTesseract();
        return;
    }
    if (!m_workerFailed && !m_workerRequestActive && startWorkerRequest())
        return;
    finishWithError(QStringLiteral("RapidOCR is unavailable"));
}

void OcrManager::startTesseract()
{
    m_stage = Stage::Recognizing;
    const QString input = (m_isPdf || m_isRegion)
        ? m_tempDir.filePath(QStringLiteral("page.ppm")) : m_path;
    const QString output = m_tempDir.filePath(QStringLiteral("ocr"));
    // PPM carries no DPI metadata, so state the render resolution explicitly.
    QStringList dpi;
    if (m_isPdf)
        dpi = {QStringLiteral("--dpi"),
               m_isRegion ? QStringLiteral("400") : QStringLiteral("200")};
    m_process.setProgram(QStringLiteral("nice"));
    m_process.setArguments(QStringList{QStringLiteral("-n"),
                            m_isRegion ? QStringLiteral("0") : QStringLiteral("10"),
                            QStringLiteral("tesseract"), input, output}
                           + dpi + QStringList{
                            QStringLiteral("-l"), QStringLiteral("eng"),
                            QStringLiteral("--psm"),
                            m_isRegion ? QStringLiteral("11")
                                       : (m_fallbackPass ? QStringLiteral("6")
                                                         : QStringLiteral("3")),
                            QStringLiteral("tsv"), QStringLiteral("makebox")});
    m_process.start();
}

bool OcrManager::startWorkerRequest()
{
    if (!m_workerAvailable)
        return false;
    if (m_worker.state() == QProcess::NotRunning) {
        m_worker.start();
        if (!m_worker.waitForStarted(100))
            return false;
    }
    const QJsonObject request{{QStringLiteral("id"), m_generation},
                              {QStringLiteral("path"),
                               (m_isPdf || m_isRegion)
                                   ? m_tempDir.filePath(QStringLiteral("page.ppm"))
                                   : m_path}};
    m_workerRequestActive = true;
    m_stage = Stage::Recognizing;
    m_worker.write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    m_worker.write("\n");
    return true;
}

void OcrManager::handleWorkerOutput()
{
    m_workerBuffer += m_worker.readAllStandardOutput();
    while (true) {
        const qsizetype newline = m_workerBuffer.indexOf('\n');
        if (newline < 0)
            return;
        const QByteArray line = m_workerBuffer.left(newline);
        m_workerBuffer.remove(0, newline + 1);
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject() || !m_workerRequestActive)
            continue;
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("id")).toInt() != m_generation)
            continue;
        m_workerRequestActive = false;
        const QString error = object.value(QStringLiteral("error")).toString();
        if (!error.isEmpty()) {
            m_workerFailed = true;
            if (m_isPdf)
                fallbackToTesseract();
            else
                finishWithError(error);
            return;
        }
        QVariantList words = object.value(QStringLiteral("words")).toArray().toVariantList();
        if (words.isEmpty() && !m_rapidMode) {
            if (m_isPdf) {
                m_workerFailed = true;
                startTesseract();
            } else {
                finishWithError(QStringLiteral("OCR produced no text output"));
            }
            return;
        }
        for (QVariant &value : words) {
            QVariantMap word = value.toMap();
            const qreal offsetX = m_isRegion ? m_region.x() : (m_rapidMode ? m_jobOffsetX : 0);
            const qreal offsetY = m_isRegion ? m_region.y() : (m_rapidMode ? m_jobOffsetY : 0);
            if (m_rapidMode)
                word[QStringLiteral("block")] = m_jobIndex;
            word[QStringLiteral("x")] = word.value(QStringLiteral("x")).toDouble() * m_scaleX + offsetX;
            word[QStringLiteral("y")] = word.value(QStringLiteral("y")).toDouble() * m_scaleY + offsetY;
            word[QStringLiteral("w")] = word.value(QStringLiteral("w")).toDouble() * m_scaleX;
            word[QStringLiteral("h")] = word.value(QStringLiteral("h")).toDouble() * m_scaleY;
            QVariantList chars = word.value(QStringLiteral("chars")).toList();
            for (QVariant &charValue : chars) {
                QVariantMap character = charValue.toMap();
                character[QStringLiteral("x")] = character.value(QStringLiteral("x")).toDouble() * m_scaleX + offsetX;
                character[QStringLiteral("y")] = character.value(QStringLiteral("y")).toDouble() * m_scaleY + offsetY;
                character[QStringLiteral("w")] = character.value(QStringLiteral("w")).toDouble() * m_scaleX;
                character[QStringLiteral("h")] = character.value(QStringLiteral("h")).toDouble() * m_scaleY;
                charValue = character;
            }
            word[QStringLiteral("chars")] = chars;
            value = word;
        }
        if (m_rapidMode) {
            m_accum.append(words);
            if (++m_jobIndex < m_jobs.size()) {
                startNextJob();
                return;
            }
            words = m_accum;   // an empty page is a valid (cached) result, not an error
        }
        setProgress(100);
        setRunning(false);
        m_stage = Stage::Idle;
        QStringList text;
        for (const QVariant &value : words)
            text.append(value.toMap().value(QStringLiteral("text")).toString());
        const Result result{text.join(QLatin1Char('\n')), words};
        if (m_cacheEnabled) {
            m_cache.insert(m_cacheKey, result);
            saveCached(m_cacheKey, result);
        }
        if (m_isRegion)
            emit regionFinished(m_generation, m_page, m_region, words, QString());
        else
            emit finished(m_generation, m_page, result.text, words, QString());
        return;
    }
}

void OcrManager::finishWithError(const QString &error)
{
    m_stage = Stage::Idle;
    setRunning(false);
    setProgress(0);
    const QString message = error.isEmpty() ? QStringLiteral("OCR failed") : error;
    if (m_isRegion)
        emit regionFinished(m_generation, m_page, m_region, {}, message);
    else
        emit finished(m_generation, m_page, QString(), QVariantList(), message);
}

void OcrManager::parseTsv()
{
    QFile file(m_tempDir.filePath(QStringLiteral("ocr.tsv")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        finishWithError(QStringLiteral("OCR produced no text output"));
        return;
    }

    QTextStream stream(&file);
    stream.readLine();
    QVariantList words;
    QStringList text;
    while (!stream.atEnd()) {
        const QStringList fields = stream.readLine().split(QLatin1Char('\t'));
        if (fields.size() < 12 || fields.at(0) != QStringLiteral("5"))
            continue;
        const QString word = fields.mid(11).join(QStringLiteral("\t")).trimmed();
        if (word.isEmpty())
            continue;
        const qreal x = fields.at(6).toDouble() * m_scaleX + (m_isRegion ? m_region.x() : 0);
        const qreal y = fields.at(7).toDouble() * m_scaleY + (m_isRegion ? m_region.y() : 0);
        const qreal w = fields.at(8).toDouble() * m_scaleX;
        const qreal h = fields.at(9).toDouble() * m_scaleY;
        words.append(QVariantMap{{QStringLiteral("text"), word},
                                 {QStringLiteral("x"), x},
                                 {QStringLiteral("y"), y},
                                 {QStringLiteral("w"), w},
                                 {QStringLiteral("h"), h},
                                 {QStringLiteral("block"), fields.at(2).toInt()},
                                 {QStringLiteral("paragraph"), fields.at(3).toInt()},
                                 {QStringLiteral("line"), fields.at(4).toInt()},
                                 {QStringLiteral("word"), fields.at(5).toInt()}});
        text.append(word);
    }
    if (words.isEmpty() && !m_isRegion && !m_fallbackPass) {
        m_fallbackPass = true;
        file.close();
        QFile::remove(m_tempDir.filePath(QStringLiteral("ocr.tsv")));
        QFile::remove(m_tempDir.filePath(QStringLiteral("ocr.box")));
        setProgress(55);
        startTesseract();
        return;
    }

    QFile boxFile(m_tempDir.filePath(QStringLiteral("ocr.box")));
    if (boxFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream boxes(&boxFile);
        while (!boxes.atEnd()) {
            const QString line = boxes.readLine();
            const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (fields.size() < 6)
                continue;
            const QString character = fields.first();
            const qreal left = fields.at(1).toDouble();
            const qreal bottom = fields.at(2).toDouble();
            const qreal right = fields.at(3).toDouble();
            const qreal top = fields.at(4).toDouble();
            const QImageReader reader((m_isPdf || m_isRegion)
                ? m_tempDir.filePath(QStringLiteral("page.ppm")) : m_path);
            const qreal imageHeight = reader.size().height();
            const qreal x = left * m_scaleX + (m_isRegion ? m_region.x() : 0);
            const qreal y = (imageHeight - top) * m_scaleY
                + (m_isRegion ? m_region.y() : 0);
            const qreal w = (right - left) * m_scaleX;
            const qreal h = (top - bottom) * m_scaleY;
            const qreal cx = x + w / 2;
            const qreal cy = y + h / 2;
            int target = -1;
            qreal best = std::numeric_limits<qreal>::max();
            for (int i = 0; i < words.size(); ++i) {
                const QVariantMap word = words.at(i).toMap();
                const qreal wx = word.value(QStringLiteral("x")).toDouble();
                const qreal wy = word.value(QStringLiteral("y")).toDouble();
                const qreal ww = word.value(QStringLiteral("w")).toDouble();
                const qreal wh = word.value(QStringLiteral("h")).toDouble();
                const qreal dx = cx < wx ? wx - cx : cx > wx + ww ? cx - wx - ww : 0;
                const qreal dy = cy < wy ? wy - cy : cy > wy + wh ? cy - wy - wh : 0;
                const qreal distance = dx * dx + dy * dy;
                if (distance < best) {
                    best = distance;
                    target = i;
                }
            }
            if (target >= 0) {
                QVariantMap word = words.at(target).toMap();
                QVariantList chars = word.value(QStringLiteral("chars")).toList();
                chars.append(QVariantMap{{QStringLiteral("text"), character},
                                         {QStringLiteral("x"), x},
                                         {QStringLiteral("y"), y},
                                         {QStringLiteral("w"), w},
                                         {QStringLiteral("h"), h}});
                word.insert(QStringLiteral("chars"), chars);
                words[target] = word;
            }
        }
    }
    setProgress(100);
    setRunning(false);
    m_stage = Stage::Idle;
    if (m_isRegion) {
        const Result result{text.join(QLatin1Char(' ')), words};
        if (m_cacheEnabled && (!m_workerIsRapid || !m_workerFailed)) {
            m_cache.insert(m_cacheKey, result);
            saveCached(m_cacheKey, result);
        }
        emit regionFinished(m_generation, m_page, m_region, words, QString());
        return;
    }
    const Result result{text.join(QLatin1Char(' ')), words};
    if (m_cacheEnabled && (!m_workerIsRapid || !m_workerFailed)) {
        m_cache.insert(m_cacheKey, result);
        saveCached(m_cacheKey, result);
    }
    emit finished(m_generation, m_page, result.text, result.words, QString());
}

QString OcrManager::cachePath(const QString &key) const
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/ocr");
    const QByteArray name = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex();
    return root + QLatin1Char('/') + QString::fromLatin1(name) + QStringLiteral(".json");
}

bool OcrManager::loadCached(const QString &key, Result &result) const
{
    QFile file(cachePath(key));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return false;
    const QJsonObject object = document.object();
    result.text = object.value(QStringLiteral("text")).toString();
    result.words = object.value(QStringLiteral("words")).toArray().toVariantList();
    return true;
}

void OcrManager::saveCached(const QString &key, const Result &result) const
{
    const QString path = cachePath(key);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    const QJsonObject object{{QStringLiteral("version"), 1},
                             {QStringLiteral("text"), result.text},
                             {QStringLiteral("words"), QJsonArray::fromVariantList(result.words)}};
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.commit();
    if (!m_cachePruned) {
        pruneCache();
        m_cachePruned = true;
    }
}

void OcrManager::pruneCache() const
{
    QDir directory(QFileInfo(cachePath(QString())).absolutePath());
    QFileInfoList files = directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files,
                                                   QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const QFileInfo &file : files)
        total += file.size();
    constexpr qint64 limit = 128LL * 1024 * 1024;
    for (const QFileInfo &file : files) {
        if (total <= limit)
            break;
        total -= file.size();
        QFile::remove(file.absoluteFilePath());
    }
}
