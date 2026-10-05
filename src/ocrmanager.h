#pragma once

#include <QObject>
#include <QProcess>
#include <QSizeF>
#include <QRectF>
#include <QTemporaryDir>
#include <QVariantList>
#include <QHash>

class OcrManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    // Read on demand (it turns false if the worker dies), not bound.
    Q_PROPERTY(bool rapidAvailable READ rapidAvailable)

public:
    explicit OcrManager(QObject *parent = nullptr);
    ~OcrManager() override;

    bool running() const { return m_running; }
    int progress() const { return m_progress; }
    // True when the RapidOCR worker is installed; PDFs are then read with it.
    bool rapidAvailable() const { return m_workerIsRapid && m_workerAvailable; }

    // For PDFs, `regions` (page-point rects: {x,y,w,h}) limits recognition to
    // those crops, e.g. the embedded images of a page that already has native
    // text. Empty = the whole page.
    Q_INVOKABLE void recognize(const QString &path, int page, QSizeF pageSize,
                               int generation, const QVariantList &regions = {});
    Q_INVOKABLE void recognizeRegion(const QString &path, int page, QSizeF pageSize,
                                     QRectF region, int generation);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void prewarm();
    void setCacheEnabled(bool enabled) { m_cacheEnabled = enabled; }

signals:
    void runningChanged();
    void progressChanged();
    void finished(int generation, int page, const QString &text,
                  const QVariantList &words, const QString &error);
    void regionFinished(int generation, int page, const QRectF &region,
                        const QVariantList &words, const QString &error);

private:
    struct Result { QString text; QVariantList words; };
    void setRunning(bool running);
    void setProgress(int progress);
    void startRecognition();
    void connectWorker();
    void startTesseract();
    void startPageRender();
    void startNextJob();
    void fallbackToTesseract();
    QString pageCacheKey(const QString &backend, const QString &tag) const;
    void finishWithError(const QString &error);
    void parseTsv();
    bool startWorkerRequest();
    void handleWorkerOutput();
    void begin(const QString &path, int page, QSizeF pageSize, int generation);
    bool loadCached(const QString &key, Result &result) const;
    void saveCached(const QString &key, const Result &result) const;
    QString cachePath(const QString &key) const;
    void pruneCache() const;

    QProcess m_process;
    QProcess m_worker;
    QByteArray m_workerBuffer;
    bool m_workerRequestActive = false;
    bool m_workerAvailable = false;
    bool m_workerFailed = false;
    bool m_workerIsRapid = false;
    QTemporaryDir m_tempDir;
    QString m_path;
    QSizeF m_pageSize;
    int m_page = -1;
    int m_generation = 0;
    int m_progress = 0;
    bool m_running = false;
    bool m_cancelled = false;
    bool m_isPdf = true;
    bool m_isRegion = false;
    bool m_fallbackPass = false;
    // RapidOCR on a PDF: one crop (or the whole page) per job, results merged.
    bool m_rapidMode = false;
    QVector<QRectF> m_jobs;
    int m_jobIndex = 0;
    qreal m_jobOffsetX = 0;
    qreal m_jobOffsetY = 0;
    QVariantList m_accum;
    QString m_regionsTag;
    QRectF m_region;
    qreal m_scaleX = 72.0 / 200.0;
    qreal m_scaleY = 72.0 / 200.0;
    enum class Stage { Idle, Rendering, Recognizing } m_stage = Stage::Idle;
    QString m_cacheKey;
    QHash<QString, Result> m_cache;
    mutable bool m_cachePruned = false;
    bool m_cacheEnabled = true;
};
