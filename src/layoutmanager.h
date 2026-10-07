#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QVector>

class Document;

// Runs the page-layout model over a document's pages in background worker
// processes and hands what it finds to the Document, which uses it to tell
// tables, figures, captions, headings, headers and footers apart. Results are
// cached on disk per document, so a page is analysed once.
class LayoutManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit LayoutManager(Document *doc, QObject *parent = nullptr);
    ~LayoutManager() override;

    // The model and its runtime are installed.
    bool available() const;
    bool busy() const { return !m_queue.isEmpty() || activeJobs() > 0; }
    void setEnabled(bool on);

    Q_INVOKABLE bool isReady(int first, int count) const;
    // Puts pages first..first+count-1 at the front of the queue. True when all of
    // them are already analysed; otherwise rangeReady() follows.
    Q_INVOKABLE bool ensure(int first, int count);
    // Queues every other page after those, nearest first.
    Q_INVOKABLE void analyzeFrom(int page);
    // Forgets queued work and ends the workers.
    Q_INVOKABLE void stop();
    void notifyAvailabilityChanged() { emit availableChanged(); }

signals:
    void availableChanged();
    void busyChanged();
    void pageAnalyzed(int page);
    void rangeReady();

private:
    struct Worker {
        QProcess *process = nullptr;
        bool ready = false;
        int page = -1;               // the page being analysed, -1 when idle
        int generation = 0;
        QString imagePath;
        QByteArray buffer;
    };

    int activeJobs() const;
    void reset();
    void pump();
    void startWorker();
    void begin(Worker &worker, int page);
    void rendered(int generation, int page, const QString &path, bool ok);
    void handleOutput(Worker &worker);
    bool loadCached(int page);
    void saveCached(int page, const QByteArray &json) const;
    QString cacheFile(int page) const;
    void checkRange();
    void enqueueFront(int page);
    void enqueueBack(int page);

    Document *m_doc;
    bool m_enabled = true;
    int m_generation = 0;
    QList<int> m_queue;
    QVector<Worker *> m_workers;
    QTemporaryDir m_tmp;
    QString m_cacheKey;
    int m_wantFirst = -1;
    int m_wantCount = 0;
    bool m_wasBusy = false;
};
