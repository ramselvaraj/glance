#pragma once

#include <QElapsedTimer>
#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVector>
#include <memory>

// Read-aloud engine: speaks a list of paragraphs sentence by sentence with a
// local Kokoro model. Synthesis runs in an isolated worker process; a few
// sentences are synthesized ahead of playback so the voice never waits.
class Reader : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool installing READ installing NOTIFY installingChanged)
    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(int paragraph READ paragraph NOTIFY positionChanged)
    Q_PROPERTY(int sentence READ sentence NOTIFY positionChanged)
    Q_PROPERTY(int unit READ unit NOTIFY positionChanged)
    // How far through the sentence being heard the voice is, 0..1.
    Q_PROPERTY(qreal unitProgress READ unitProgress NOTIFY progressChanged)
    Q_PROPERTY(QString voice READ voice WRITE setVoice NOTIFY voiceChanged)
    Q_PROPERTY(qreal speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(QStringList voices READ voices NOTIFY voicesChanged)

public:
    enum State { Idle, Loading, Playing, Paused };
    Q_ENUM(State)

    explicit Reader(QObject *parent = nullptr);
    ~Reader() override;

    // The runtime and model files are installed (read aloud is opt-in).
    bool available() const;
    bool installing() const { return m_installer.state() != QProcess::NotRunning; }
    State state() const { return m_state; }
    int paragraph() const { return m_paragraph; }
    int sentence() const { return m_sentence; }
    int unit() const { return m_current; }
    qreal unitProgress() const { return m_progress; }
    QString voice() const { return m_voice; }
    qreal speed() const { return m_speed; }
    QStringList voices() const { return m_voices; }
    void setVoice(const QString &voice);
    void setSpeed(qreal speed);

    // False while AI features are switched off: nothing starts, no process runs.
    void setEnabled(bool on);
    // Stops everything and ends the synthesis process.
    Q_INVOKABLE void shutdown();
    // Deletes the downloaded runtime and model. Returns whether it is gone.
    Q_INVOKABLE bool removeModel();
    // Size of what read aloud has downloaded, e.g. "322 MB" ("" when nothing).
    Q_INVOKABLE QString diskUsage() const;
    // Downloads the Kokoro runtime and model (about 120 MB) into the user's data dir.
    Q_INVOKABLE void install();
    // Sentences of a paragraph, exactly as they are spoken one at a time.
    // `kind` is the paragraph's kind: a heading is one unit, never split.
    Q_INVOKABLE static QStringList splitSentences(const QString &paragraph, const QString &kind = QString());
    // What of `sentence` is actually spoken (citations, links and the like removed).
    Q_INVOKABLE static QString speakable(const QString &sentence, const QString &kind = QString())
    {
        return kind == QLatin1String("heading") ? cleanForSpeech(speechForHeading(sentence))
                                                : cleanForSpeech(sentence);
    }
    // The same sentences as [{start, end}] character ranges in `paragraph`.
    Q_INVOKABLE static QVariantList sentenceRanges(const QString &paragraph, const QString &kind = QString());
    // What is being spoken, one entry per sentence: [{paragraph, sentence, text}].
    // `text` is the cleaned wording ("" for fragments with nothing to say).
    Q_INVOKABLE QVariantList units() const;
    // Starts reading `paragraphs` at `index`.
    // `kinds` (parallel to `paragraphs`, optional) lets headings and list items be
    // spoken and paced differently from running text.
    Q_INVOKABLE void start(const QStringList &paragraphs, int index = 0,
                           const QStringList &kinds = QStringList());
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void jumpToParagraph(int paragraph);
    Q_INVOKABLE void nextParagraph();
    Q_INVOKABLE void previousParagraph();   // restarts the current one after a moment in
    Q_INVOKABLE void nextSentence();
    Q_INVOKABLE void previousSentence();

signals:
    void availableChanged();
    void installingChanged();
    void installOutput(const QString &line);
    void installFinished(bool ok);
    void stateChanged();
    void positionChanged();
    void unitsChanged();
    void progressChanged();
    void voiceChanged();
    void speedChanged();
    void voicesChanged();
    void finished();                 // reached the end of the last paragraph
    void errorOccurred(const QString &message);

private:
    struct Unit {
        int paragraph = 0;
        int sentence = 0;
        QString text;                // cleaned for speech; empty = nothing to say
        qint64 endByte = 0;          // cumulative end offset once queued for playback
        qint64 audioBytes = 0;       // length of the speech itself, without the pause after it
        qint64 speechStart = 0;      // where the voice really starts and stops inside the audio
        qint64 speechEnd = 0;        // (bytes), leaving out the silence at either end
        QString kind;                // body, heading...
        bool listItem = false;       // a bullet or numbered item: shorter gap after it
    };

    static QString dataDir();
    static QString cleanForSpeech(const QString &sentence);
    static QString speechForHeading(const QString &heading);
    void setState(State state);
    void restartAt(int unit);
    void ensureWorker();
    void requestMore();
    void handleWorkerOutput();
    void handleAudio(int unitIndex, int rate, const QByteArray &pcm);
    bool startPlayer(int rate);
    void closePlayer(bool drain);
    qint64 playedBytes();
    void pump();
    void finishPlayback();
    void setPosition(int unit);
    int firstUnitOf(int paragraph) const;

    QVector<Unit> m_units;
    int m_paragraphCount = 0;
    int m_nextRequest = 0;           // next unit to synthesize
    int m_generation = 0;
    int m_inflightUnit = -1;
    int m_inflightGeneration = 0;
    int m_current = -1;              // unit being heard
    int m_paragraph = -1;
    int m_sentence = -1;
    qreal m_progress = 0;

    QProcess m_worker;
    QByteArray m_buffer;
    bool m_workerReady = false;
    qint64 m_expectBytes = -1;       // PCM bytes still owed after a header
    int m_expectUnit = -1;
    int m_expectRate = 24000;
    int m_expectGeneration = 0;
    QProcess m_installer;

    // Audio goes to a system player (pw-play, paplay or aplay) over a pipe. It
    // plays in real time, so what has been heard is tracked with a clock that
    // is re-based whenever the player runs dry.
    QProcess *m_player = nullptr;
    QByteArray m_pending;            // synthesized audio not yet handed to the player
    qint64 m_queuedBytes = 0;        // total bytes ever queued in this player session
    qint64 m_writtenBytes = 0;
    qint64 m_playedBase = 0;         // bytes heard at the last clock re-base
    qint64 m_clockBase = 0;          // m_clock time of that re-base, in ms
    bool m_clockRunning = false;
    QElapsedTimer m_clock;
    int m_rate = 24000;
    QTimer m_pump;

    bool m_enabled = true;
    State m_state = Idle;
    QString m_voice = QStringLiteral("af_heart");
    qreal m_speed = 1.0;
    QStringList m_voices;
};
