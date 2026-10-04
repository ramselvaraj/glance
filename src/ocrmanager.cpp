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
            startTesseract();
        } else {
            setProgress(95);
            parseTsv();
        }
    });
}

void OcrManager::recognize(const QString &path, int page, QSizeF pageSize,
                           int generation)
{
    cancel();
    begin(path, page, pageSize, generation);
    m_isRegion = false;
    m_fallbackPass = false;
    m_region = QRectF();
    m_cacheKey = QStringLiteral("v6|tesseract-5|%1|%2|%3|%4|%5|eng|200")
        .arg(QFileInfo(path).canonicalFilePath())
        .arg(QFileInfo(path).size())
        .arg(QFileInfo(path).lastModified().toMSecsSinceEpoch())
        .arg(page)
        .arg(QString::number(pageSize.width(), 'f', 2)
             + QLatin1Char('x') + QString::number(pageSize.height(), 'f', 2));
    Result result;
    const auto cached = m_cache.constFind(m_cacheKey);
    if (cached != m_cache.cend() || loadCached(m_cacheKey, result)) {
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

    QFile::remove(m_tempDir.filePath(QStringLiteral("page.png")));
    QFile::remove(m_tempDir.filePath(QStringLiteral("ocr.tsv")));
    QFile::remove(m_tempDir.filePath(QStringLiteral("ocr.box")));

    if (!m_isPdf) {
        setProgress(45);
        startTesseract();
        return;
    }
    m_stage = Stage::Rendering;
    const QString prefix = m_tempDir.filePath(QStringLiteral("page"));
    m_process.setProgram(QStringLiteral("nice"));
    m_process.setArguments({QStringLiteral("-n"), QStringLiteral("10"),
                            QStringLiteral("pdftoppm"), QStringLiteral("-f"),
                            QString::number(page + 1), QStringLiteral("-l"),
                            QString::number(page + 1), QStringLiteral("-r"),
                            QStringLiteral("200"), QStringLiteral("-png"),
                            QStringLiteral("-singlefile"), path, prefix});
    m_process.start();
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
    m_fallbackPass = false;
    m_region = region.intersected(QRectF(QPointF(0, 0), pageSize));
    if (m_region.isEmpty()) {
        emit regionFinished(generation, page, m_region, {}, QStringLiteral("Empty OCR region"));
        return;
    }
    m_scaleX = 72.0 / 400.0;
    m_scaleY = 72.0 / 400.0;
    QFile::remove(m_tempDir.filePath(QStringLiteral("page.png")));
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
        if (crop.isNull() || !crop.save(m_tempDir.filePath(QStringLiteral("page.png")))) {
            finishWithError(QStringLiteral("Could not prepare OCR region"));
            return;
        }
        m_scaleX = m_region.width() / crop.width();
        m_scaleY = m_region.height() / crop.height();
        setRunning(true);
        setProgress(45);
        startTesseract();
        return;
    }
    setRunning(true);
    setProgress(5);
    m_stage = Stage::Rendering;
    const qreal pixelsPerPoint = 400.0 / 72.0;
    m_process.setProgram(QStringLiteral("nice"));
    m_process.setArguments({QStringLiteral("-n"), QStringLiteral("10"),
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
                            QStringLiteral("-png"), QStringLiteral("-singlefile"),
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
    setRunning(false);
    setProgress(0);
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

void OcrManager::startTesseract()
{
    m_stage = Stage::Recognizing;
    const QString input = (m_isPdf || m_isRegion)
        ? m_tempDir.filePath(QStringLiteral("page.png")) : m_path;
    const QString output = m_tempDir.filePath(QStringLiteral("ocr"));
    m_process.setProgram(QStringLiteral("nice"));
    m_process.setArguments({QStringLiteral("-n"), QStringLiteral("10"),
                            QStringLiteral("tesseract"), input, output,
                            QStringLiteral("-l"), QStringLiteral("eng"),
                            QStringLiteral("--psm"),
                            m_isRegion ? QStringLiteral("11")
                                       : (m_fallbackPass ? QStringLiteral("6")
                                                         : QStringLiteral("3")),
                            QStringLiteral("tsv"), QStringLiteral("makebox")});
    m_process.start();
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
        if (m_isRegion && fields.at(10).toDouble() < 30.0)
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
                ? m_tempDir.filePath(QStringLiteral("page.png")) : m_path);
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
        emit regionFinished(m_generation, m_page, m_region, words, QString());
        return;
    }
    const Result result{text.join(QLatin1Char(' ')), words};
    m_cache.insert(m_cacheKey, result);
    saveCached(m_cacheKey, result);
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
    pruneCache();
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
