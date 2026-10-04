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

public:
    explicit OcrManager(QObject *parent = nullptr);

    bool running() const { return m_running; }
    int progress() const { return m_progress; }

    Q_INVOKABLE void recognize(const QString &path, int page, QSizeF pageSize,
                               int generation);
    Q_INVOKABLE void recognizeRegion(const QString &path, int page, QSizeF pageSize,
                                     QRectF region, int generation);
    Q_INVOKABLE void cancel();

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
    void startTesseract();
    void finishWithError(const QString &error);
    void parseTsv();
    void begin(const QString &path, int page, QSizeF pageSize, int generation);
    bool loadCached(const QString &key, Result &result) const;
    void saveCached(const QString &key, const Result &result) const;
    QString cachePath(const QString &key) const;
    void pruneCache() const;

    QProcess m_process;
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
    QRectF m_region;
    qreal m_scaleX = 72.0 / 200.0;
    qreal m_scaleY = 72.0 / 200.0;
    enum class Stage { Idle, Rendering, Recognizing } m_stage = Stage::Idle;
    QString m_cacheKey;
    QHash<QString, Result> m_cache;
};
