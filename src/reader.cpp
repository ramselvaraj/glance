#include "reader.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextBoundaryFinder>
#include <signal.h>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kPrefetchUnits = 3;          // sentences synthesized ahead of the one playing
// Gaps are what let a listener take in what was just said. They are scaled by
// the speaking speed so that fast reading keeps its proportions.
// Between handing audio to the player and hearing it: the player's own buffer
// plus the audio device. Without allowing for it, highlights run ahead of the voice.
constexpr int kOutputLatencyMs = 230;
constexpr int kSentencePauseMs = 280;
constexpr int kParagraphPauseMs = 1000;
constexpr int kListItemPauseMs = 650;
constexpr int kBeforeHeadingMs = 1600;
constexpr int kAfterHeadingMs = 1100;
constexpr int kLeadMs = 300;               // audio kept queued in the player

QString workerScript()
{
    const QString installed = QCoreApplication::applicationDirPath()
        + QStringLiteral("/glance-kokoro-worker");
    return QFileInfo::exists(installed) ? installed : QStringLiteral(KOKORO_WORKER_SOURCE_S);
}

QString setupScript()
{
    const QString installed = QCoreApplication::applicationDirPath()
        + QStringLiteral("/glance-setup-tts");
    return QFileInfo::exists(installed) ? installed : QStringLiteral(SETUP_TTS_SOURCE_S);
}

// Where the voice is actually heard inside a synthesized sentence: Kokoro pads
// both ends with silence, and the size of the padding varies.
void speechBounds(const QByteArray &pcm, int rate, qint64 &start, qint64 &end)
{
    const qint16 *s = reinterpret_cast<const qint16 *>(pcm.constData());
    const qint64 n = pcm.size() / 2;
    const qint64 window = std::max(1, rate / 100);   // 10 ms
    constexpr int kThreshold = 400;                  // about -38 dBFS
    start = 0;
    end = pcm.size();
    qint64 first = -1, last = -1;
    for (qint64 i = 0; i + window <= n; i += window) {
        int peak = 0;
        for (qint64 k = 0; k < window; ++k)
            peak = std::max(peak, std::abs(int(s[i + k])));
        if (peak > kThreshold) {
            if (first < 0)
                first = i;
            last = i + window;
        }
    }
    if (first >= 0 && last > first) {
        start = first * 2;
        end = last * 2;
    }
}

bool endsWithAbbreviation(const QString &s)
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:\b(?:et al|e\.g|i\.e|etc|vs|cf|fig|figs|eq|eqs|sec|ref|refs|no|nos|dr|mr|mrs|ms|prof|st|approx|resp|vol|pp?)|\b[A-Z])\.\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(s).hasMatch();
}

} // namespace

Reader::Reader(QObject *parent)
    : QObject(parent)
{
    m_pump.setInterval(20);
    connect(&m_pump, &QTimer::timeout, this, &Reader::pump);

    connect(&m_worker, &QProcess::readyReadStandardOutput, this, &Reader::handleWorkerOutput);
    connect(&m_worker, &QProcess::finished, this, [this] {
        m_workerReady = false;
        m_inflightUnit = -1;
        if (m_state == Loading || m_state == Playing) {
            emit errorOccurred(QStringLiteral("The speech engine stopped unexpectedly"));
            stop();
        }
    });
    m_worker.setStandardErrorFile(QProcess::nullDevice());

    connect(&m_installer, &QProcess::readyReadStandardOutput, this, [this] {
        for (const QByteArray &line : m_installer.readAllStandardOutput().split('\n'))
            if (!line.trimmed().isEmpty())
                emit installOutput(QString::fromUtf8(line).trimmed());
    });
    connect(&m_installer, &QProcess::readyReadStandardError, this, [this] {
        for (const QByteArray &line : m_installer.readAllStandardError().split('\n'))
            if (!line.trimmed().isEmpty())
                emit installOutput(QString::fromUtf8(line).trimmed());
    });
    connect(&m_installer, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        emit installingChanged();
        emit availableChanged();
        emit installFinished(status == QProcess::NormalExit && code == 0 && available());
    });
}

Reader::~Reader()
{
    m_worker.closeWriteChannel();
    if (!m_worker.waitForFinished(500))
        m_worker.kill();
    if (m_installer.state() != QProcess::NotRunning)
        m_installer.kill();
}

QString Reader::dataDir()
{
    const QString override = qEnvironmentVariable("GLANCE_TTS_DIR");
    if (!override.isEmpty())
        return override;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/glance/tts");
}

bool Reader::available() const
{
    const QString dir = dataDir();
    return QFileInfo::exists(dir + QStringLiteral("/runtime/bin/python"))
        && QFileInfo::exists(dir + QStringLiteral("/kokoro-v1.0.onnx"))
        && QFileInfo::exists(dir + QStringLiteral("/voices-v1.0.bin"))
        && QFileInfo::exists(workerScript());
}

void Reader::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    if (!on)
        shutdown();
}

void Reader::shutdown()
{
    stop();
    if (m_installer.state() != QProcess::NotRunning) {
        m_installer.kill();
        m_installer.waitForFinished(1000);
    }
    if (m_worker.state() != QProcess::NotRunning) {
        m_worker.closeWriteChannel();
        if (!m_worker.waitForFinished(800))
            m_worker.kill();
    }
    m_workerReady = false;
}

bool Reader::removeModel()
{
    shutdown();
    const QString dir = dataDir();
    // Only ever delete a directory that is plainly ours.
    if (!dir.endsWith(QStringLiteral("/tts")) && qEnvironmentVariableIsEmpty("GLANCE_TTS_DIR"))
        return false;
    const bool ok = !QFileInfo::exists(dir) || QDir(dir).removeRecursively();
    emit availableChanged();
    return ok;
}

QString Reader::diskUsage() const
{
    qint64 total = 0;
    QDirIterator it(dataDir(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    if (total <= 0)
        return QString();
    return total >= (1 << 30) ? QStringLiteral("%1 GB").arg(total / double(1 << 30), 0, 'f', 1)
                              : QStringLiteral("%1 MB").arg(total / (1 << 20));
}

void Reader::install()
{
    if (installing() || !m_enabled)
        return;
    QDir().mkpath(dataDir());
    m_installer.start(QStringLiteral("sh"), {setupScript(), dataDir()});
    emit installingChanged();
}

QStringList Reader::splitSentences(const QString &paragraph, const QString &kind)
{
    if (kind == QLatin1String("heading")) {
        const QString whole = paragraph.trimmed();
        return whole.isEmpty() ? QStringList() : QStringList{whole};
    }
    QStringList out;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, paragraph);
    int start = 0;
    for (int end = finder.toNextBoundary(); end != -1; end = finder.toNextBoundary()) {
        const QString piece = paragraph.mid(start, end - start).trimmed();
        start = end;
        if (piece.isEmpty())
            continue;
        // "et al." / "Fig." / "e.g." are not sentence ends.
        if (!out.isEmpty() && endsWithAbbreviation(out.last()))
            out.last() += QLatin1Char(' ') + piece;
        else
            out.append(piece);
    }
    // Keep each spoken unit short enough to synthesize quickly: split long ones at clauses.
    QStringList bounded;
    for (const QString &s : std::as_const(out)) {
        QString rest = s;
        while (rest.size() > 280) {
            int cut = -1;
            for (const QString &sep : {QStringLiteral("; "), QStringLiteral(", "), QStringLiteral(" ")}) {
                cut = rest.lastIndexOf(sep, 240);
                if (cut > 80)
                    break;
                cut = -1;
            }
            if (cut < 0)
                cut = 240;
            bounded.append(rest.left(cut + 1).trimmed());
            rest = rest.mid(cut + 1).trimmed();
        }
        if (!rest.isEmpty())
            bounded.append(rest);
    }
    return bounded;
}

QVariantList Reader::sentenceRanges(const QString &paragraph, const QString &kind)
{
    QVariantList out;
    int pos = 0;
    for (const QString &s : splitSentences(paragraph, kind)) {
        int at = paragraph.indexOf(s, pos);
        if (at < 0)
            at = pos;   // a merged sentence whose gap differs: fall back to the running offset
        out.append(QVariantMap{{QStringLiteral("start"), at},
                               {QStringLiteral("end"), std::min<int>(paragraph.size(), at + s.size())}});
        pos = at + s.size();
    }
    return out;
}

// A heading read aloud as written is abrupt: "3.2 DESIGN ELABORATION" becomes
// the title "Design Elaboration." and ends with a falling tone.
QString Reader::speechForHeading(const QString &heading)
{
    static const QRegularExpression numbering(
        QStringLiteral(R"(^(?:(?:section|chapter|part|appendix)\s+)?(?:\d+(?:\.\d+)*\.?|[IVXLC]+\.|[A-Z]\.)\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    QString s = heading.simplified();
    s.remove(numbering);
    bool hasLower = false;
    for (const QChar ch : std::as_const(s))
        if (ch.isLower()) {
            hasLower = true;
            break;
        }
    if (!hasLower && !s.isEmpty()) {
        // ALL CAPS would be spelled out letter by letter.
        static const QStringList small = {QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("and"),
                                          QStringLiteral("the"), QStringLiteral("of"), QStringLiteral("for"),
                                          QStringLiteral("in"), QStringLiteral("on"), QStringLiteral("to"),
                                          QStringLiteral("with"), QStringLiteral("vs")};
        QStringList words = s.toLower().split(QLatin1Char(' '));
        for (int i = 0; i < words.size(); ++i)
            if (i == 0 || !small.contains(words[i]))
                words[i][0] = words[i][0].toUpper();
        s = words.join(QLatin1Char(' '));
    }
    if (!s.isEmpty() && !QStringLiteral(".!?:").contains(s.back()))
        s += QLatin1Char('.');
    return s;
}

QString Reader::cleanForSpeech(const QString &sentence)
{
    // Citations that are noise when heard: [12], [3, 7-9], (Smith et al., 2020),
    // (Smith and Lee 2019; Chen 2021), (see Fig. 3), and bare identifiers.
    static const QRegularExpression numeric(
        QStringLiteral(R"(\s*\[\d+(?:\s*[,–\-]\s*\d+)*\])"));
    static const QRegularExpression authorYear(QStringLiteral(
        R"(\s*\((?:see |e\.g\.,? |cf\. )?[A-Z][\p{L}'\-]+(?:,? (?:et al\.?|and|&) ?[A-Z]?[\p{L}'\-]*)*,? \(?\d{4}[a-z]?\)?)"
        R"((?:[;,] ?[A-Z][\p{L}'\-]+(?:,? (?:et al\.?|and|&) ?[A-Z]?[\p{L}'\-]*)*,? \d{4}[a-z]?)*\))"),
        QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression url(QStringLiteral(R"((?:https?://|www\.)\S+)"));
    static const QRegularExpression doi(QStringLiteral(R"(\b(?:doi:\s*)?10\.\d{4,9}/\S+)"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression email(QStringLiteral(R"(\b[\w.+\-]+@[\w\-]+(?:\.[\w\-]+)+\b)"));
    static const QRegularExpression bullet(QStringLiteral(R"(^[\s•▪◦●*\-–—]+)"));
    QString s = sentence;
    s.replace(numeric, QString());
    s.replace(authorYear, QString());
    s.replace(email, QStringLiteral("email address"));
    s.replace(url, QStringLiteral("link"));
    s.replace(doi, QStringLiteral("link"));
    s.remove(bullet);
    s = s.simplified();
    // Nothing pronounceable (just symbols or digits stripped of context).
    bool speakable = false;
    for (const QChar c : std::as_const(s))
        if (c.isLetter() || c.isDigit()) {
            speakable = true;
            break;
        }
    return speakable ? s : QString();
}

void Reader::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void Reader::setVoice(const QString &voice)
{
    if (voice == m_voice)
        return;
    m_voice = voice;
    emit voiceChanged();
    if (m_state != Idle && m_current >= 0)
        restartAt(m_current);
}

void Reader::setSpeed(qreal speed)
{
    speed = std::clamp(speed, 0.5, 2.0);
    if (qFuzzyCompare(speed, m_speed))
        return;
    m_speed = speed;
    emit speedChanged();
    if (m_state != Idle && m_current >= 0)
        restartAt(m_current);
}

void Reader::ensureWorker()
{
    if (m_worker.state() != QProcess::NotRunning)
        return;
    const QString dir = dataDir();
    m_buffer.clear();
    m_workerReady = false;
    m_expectBytes = -1;
    m_inflightUnit = -1;
    m_worker.start(dir + QStringLiteral("/runtime/bin/python"),
                   {workerScript(), dir + QStringLiteral("/kokoro-v1.0.onnx"),
                    dir + QStringLiteral("/voices-v1.0.bin")});
}

int Reader::firstUnitOf(int paragraph) const
{
    for (int i = 0; i < m_units.size(); ++i)
        if (m_units[i].paragraph >= paragraph)
            return i;
    return m_units.size();
}

QVector<Reader::Unit> Reader::buildUnits(const QStringList &paragraphs, const QStringList &kinds) const
{
    QVector<Unit> units;
    static const QRegularExpression listMarker(QStringLiteral(R"(^\s*(?:[•▪◦●*\-–—]|\(?\d{1,2}[.)]|\(?[a-z][.)])\s)"));
    for (int p = 0; p < paragraphs.size(); ++p) {
        const QString kind = p < kinds.size() ? kinds[p] : QString();
        const bool item = kind != QLatin1String("heading") && listMarker.match(paragraphs[p]).hasMatch();
        const QStringList sentences = splitSentences(paragraphs[p], kind);
        for (int s = 0; s < sentences.size(); ++s) {
            Unit u;
            u.paragraph = p;
            u.sentence = s;
            u.text = kind == QLatin1String("heading") ? cleanForSpeech(speechForHeading(sentences[s]))
                                                      : cleanForSpeech(sentences[s]);
            u.kind = kind;
            u.listItem = item;
            units.append(u);
        }
    }
    return units;
}

bool Reader::canRefresh() const
{
    return (m_state == Playing || m_state == Paused) && m_current >= 0 && m_current < m_units.size()
        && m_inflightUnit < 0 && m_expectBytes < 0;
}

bool Reader::refresh(const QStringList &paragraphs, const QStringList &kinds, int paragraph, int sentence)
{
    if (!canRefresh())
        return false;
    QVector<Unit> units = buildUnits(paragraphs, kinds);
    const Unit &heard = m_units[m_current];
    int target = -1;
    for (int i = 0; i < units.size(); ++i)
        if (units[i].paragraph == paragraph && units[i].sentence == sentence
            && units[i].text == heard.text) {
            target = i;
            break;
        }
    if (target < 0)
        return false;
    const int shift = target - m_current;
    // Everything already synthesized (the sentences from the one heard up to the
    // next to be requested) has to be the same text in the new list.
    for (int k = m_current; k < m_nextRequest; ++k) {
        const int nk = k + shift;
        if (nk < 0 || nk >= units.size() || units[nk].text != m_units[k].text)
            return false;
    }
    for (int k = m_current; k < m_nextRequest; ++k) {
        const Unit &old = m_units[k];
        Unit &u = units[k + shift];
        u.endByte = old.endByte;
        u.startByte = old.startByte;
        u.audioBytes = old.audioBytes;
        u.speechStart = old.speechStart;
        u.speechEnd = old.speechEnd;
    }
    m_units = units;
    m_paragraphCount = paragraphs.size();
    m_current = target;
    m_nextRequest += shift;
    m_paragraph = paragraph;
    m_sentence = sentence;
    emit unitsChanged();
    emit positionChanged();
    return true;
}

void Reader::start(const QStringList &paragraphs, int index, const QStringList &kinds)
{
    if (!m_enabled)
        return;
    if (!available()) {
        emit errorOccurred(QStringLiteral("Read aloud is not installed"));
        return;
    }
    m_units = buildUnits(paragraphs, kinds);
    m_paragraphCount = paragraphs.size();
    emit unitsChanged();
    if (m_units.isEmpty()) {
        emit finished();
        return;
    }
    restartAt(firstUnitOf(std::clamp(index, 0, std::max(0, m_paragraphCount - 1))));
}

QVariantList Reader::units() const
{
    QVariantList out;
    out.reserve(m_units.size());
    for (const Unit &u : m_units)
        out.append(QVariantMap{{QStringLiteral("paragraph"), u.paragraph},
                               {QStringLiteral("sentence"), u.sentence},
                               {QStringLiteral("text"), u.text}});
    return out;
}

void Reader::restartAt(int unit)
{
    if (unit >= m_units.size()) {
        finishPlayback();
        return;
    }
    ++m_generation;
    m_pending.clear();
    m_queuedBytes = 0;
    m_writtenBytes = 0;
    m_pump.stop();
    closePlayer(false);
    for (Unit &u : m_units)
        u.endByte = 0;
    m_nextRequest = std::max(0, unit);
    m_current = m_nextRequest;
    setPosition(m_current);
    setState(Loading);
    ensureWorker();
    requestMore();
}

void Reader::setPosition(int unit)
{
    m_current = unit;
    const int p = unit >= 0 && unit < m_units.size() ? m_units[unit].paragraph : -1;
    const int s = unit >= 0 && unit < m_units.size() ? m_units[unit].sentence : -1;
    if (p == m_paragraph && s == m_sentence)
        return;
    m_paragraph = p;
    m_sentence = s;
    emit positionChanged();
}

void Reader::requestMore()
{
    if (!m_workerReady || m_inflightUnit >= 0 || m_state == Idle)
        return;
    // Skip units with nothing to say; they still get a (silent) slot so the position advances.
    while (m_nextRequest < m_units.size() && m_units[m_nextRequest].text.isEmpty()) {
        handleAudio(m_nextRequest, m_rate, QByteArray());
        ++m_nextRequest;
    }
    if (m_nextRequest >= m_units.size())
        return;
    // Do not run further ahead of playback than a few sentences.
    int ahead = 0;
    for (int i = std::max(m_current, 0); i < m_nextRequest; ++i)
        ++ahead;
    if (ahead >= kPrefetchUnits)
        return;
    m_inflightUnit = m_nextRequest;
    m_inflightGeneration = m_generation;
    const QJsonObject req{{QStringLiteral("id"), m_nextRequest},
                          {QStringLiteral("text"), m_units[m_nextRequest].text},
                          {QStringLiteral("voice"), m_voice},
                          {QStringLiteral("speed"),
                           m_units[m_nextRequest].kind == QLatin1String("heading") ? m_speed * 0.9 : m_speed}};
    ++m_nextRequest;
    m_worker.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
}

void Reader::handleWorkerOutput()
{
    m_buffer += m_worker.readAllStandardOutput();
    for (;;) {
        if (m_expectBytes >= 0) {
            if (m_buffer.size() < m_expectBytes)
                return;
            const QByteArray pcm = m_buffer.left(m_expectBytes);
            m_buffer.remove(0, m_expectBytes);
            const int unit = m_expectUnit;
            const int generation = m_expectGeneration;
            const int rate = m_expectRate;
            m_expectBytes = -1;
            if (m_inflightUnit == unit)
                m_inflightUnit = -1;
            if (generation == m_generation)
                handleAudio(unit, rate, pcm);
            requestMore();
            continue;
        }
        const int nl = m_buffer.indexOf('\n');
        if (nl < 0)
            return;
        const QJsonObject obj = QJsonDocument::fromJson(m_buffer.left(nl)).object();
        m_buffer.remove(0, nl + 1);
        if (obj.value(QStringLiteral("ready")).toBool()) {
            m_workerReady = true;
            QStringList names;
            for (const QJsonValue &v : obj.value(QStringLiteral("voices")).toArray())
                names.append(v.toString());
            if (names != m_voices) {
                m_voices = names;
                emit voicesChanged();
            }
            requestMore();
        } else if (obj.contains(QStringLiteral("error"))) {
            m_inflightUnit = -1;
            emit errorOccurred(obj.value(QStringLiteral("error")).toString());
            // Skip the sentence that failed instead of stalling the queue.
            requestMore();
        } else if (obj.contains(QStringLiteral("bytes"))) {
            m_expectUnit = obj.value(QStringLiteral("id")).toInt(-1);
            m_expectBytes = obj.value(QStringLiteral("bytes")).toInt();
            m_expectRate = obj.value(QStringLiteral("rate")).toInt(24000);
            m_expectGeneration = m_inflightGeneration;
        }
    }
}

bool Reader::startPlayer(int rate)
{
    struct Candidate { const char *program; QStringList args; };
    const QString r = QString::number(rate);
    const Candidate candidates[] = {
        {"pw-play", {QStringLiteral("--raw"), QStringLiteral("--rate=") + r, QStringLiteral("--channels=1"),
                     QStringLiteral("--format=s16"), QStringLiteral("--latency=100ms"),
                     QStringLiteral("-")}},
        {"paplay", {QStringLiteral("--raw"), QStringLiteral("--rate=") + r,
                    QStringLiteral("--channels=1"), QStringLiteral("--format=s16le"),
                    QStringLiteral("--latency-msec=100")}},
        {"aplay", {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("raw"),
                   QStringLiteral("-r"), r, QStringLiteral("-c"), QStringLiteral("1"),
                   QStringLiteral("-f"), QStringLiteral("S16_LE"), QStringLiteral("-")}},
    };
    for (const Candidate &c : candidates) {
        const QString exe = QStandardPaths::findExecutable(QString::fromLatin1(c.program));
        if (exe.isEmpty())
            continue;
        auto *p = new QProcess(this);
        p->setStandardOutputFile(QProcess::nullDevice());
        p->setStandardErrorFile(QProcess::nullDevice());
        connect(p, &QProcess::finished, this, [this, p] {
            if (p == m_player) {
                // The player died mid-session (device gone, killed by the user).
                m_player = nullptr;
                if (m_state == Loading || m_state == Playing) {
                    emit errorOccurred(QStringLiteral("The audio player stopped unexpectedly"));
                    stop();
                }
            }
            p->deleteLater();
        });
        p->start(exe, c.args);
        if (!p->waitForStarted(1000)) {
            delete p;
            continue;
        }
        m_player = p;
        m_rate = rate;
        m_clock.start();
        m_clockBase = 0;
        m_playedBase = 0;
        m_clockRunning = false;
        m_pump.start();
        return true;
    }
    emit errorOccurred(QStringLiteral("No audio player found (install pipewire, pulseaudio or alsa-utils)"));
    return false;
}

// With `drain` the player is left to finish what it already has.
void Reader::closePlayer(bool drain)
{
    QProcess *p = m_player;
    m_player = nullptr;
    m_clockRunning = false;
    if (!p)
        return;
    if (drain) {
        p->closeWriteChannel();
        QTimer::singleShot(3000, p, [p] {
            if (p->state() != QProcess::NotRunning)
                p->kill();
        });
    } else {
        p->disconnect(this);
        ::kill(p->processId(), SIGCONT);   // a paused player ignores SIGTERM until resumed
        p->kill();
        connect(p, &QProcess::finished, p, &QObject::deleteLater);
    }
}

qint64 Reader::playedBytes()
{
    if (!m_clockRunning)
        return m_playedBase;
    const qint64 now = m_clock.elapsed();
    qint64 played = m_playedBase + (now - m_clockBase) * m_rate * 2 / 1000;
    if (played >= m_writtenBytes) {
        // Ran dry: nothing more to hear until new audio is written.
        m_playedBase = played = m_writtenBytes;
        m_clockBase = now;
    }
    return played;
}

void Reader::handleAudio(int unitIndex, int rate, const QByteArray &pcm)
{
    if (unitIndex < 0 || unitIndex >= m_units.size())
        return;
    if (!m_player && !startPlayer(rate)) {
        stop();
        return;
    }
    QByteArray audio = pcm;
    const bool lastOfParagraph = unitIndex + 1 >= m_units.size()
        || m_units[unitIndex + 1].paragraph != m_units[unitIndex].paragraph;
    const Unit &unit = m_units[unitIndex];
    int pauseMs = kSentencePauseMs;
    if (lastOfParagraph) {
        const bool nextIsHeading = unitIndex + 1 < m_units.size()
            && m_units[unitIndex + 1].kind == QLatin1String("heading");
        if (unit.kind == QLatin1String("heading"))
            pauseMs = kAfterHeadingMs;
        else if (nextIsHeading)
            pauseMs = kBeforeHeadingMs;
        else if (unit.listItem)
            pauseMs = kListItemPauseMs;
        else
            pauseMs = kParagraphPauseMs;
    }
    pauseMs = int(pauseMs / std::sqrt(std::max(0.75, m_speed)));
    audio.append(QByteArray(rate * pauseMs / 1000 * 2, '\0'));
    m_pending += audio;
    m_units[unitIndex].startByte = m_queuedBytes;
    m_queuedBytes += audio.size();
    m_units[unitIndex].endByte = m_queuedBytes;
    m_units[unitIndex].audioBytes = pcm.size();
    speechBounds(pcm, rate, m_units[unitIndex].speechStart, m_units[unitIndex].speechEnd);
    if (qEnvironmentVariableIsSet("GLANCE_TTS_DEBUG"))
        qInfo("unit %d: audio %.2fs, voice %.2fs..%.2fs", unitIndex, pcm.size() / 2.0 / rate,
              m_units[unitIndex].speechStart / 2.0 / rate, m_units[unitIndex].speechEnd / 2.0 / rate);
    if (m_state == Loading)
        setState(Playing);
}

void Reader::pump()
{
    if (!m_player || m_state == Paused)
        return;
    const qint64 played = playedBytes();
    // Keep only a short lead in the player so position tracking stays accurate
    // and a skip or pause takes effect quickly.
    const qint64 leadTarget = qint64(m_rate) * 2 * kLeadMs / 1000;
    const qint64 room = leadTarget - (m_writtenBytes - played);
    if (room > 0 && !m_pending.isEmpty() && m_player->bytesToWrite() < leadTarget) {
        const qint64 n = std::min<qint64>(room, m_pending.size()) & ~qint64(1);
        if (n > 0) {
            if (!m_clockRunning) {
                m_clockRunning = true;
                m_clockBase = m_clock.elapsed();
            }
            m_player->write(m_pending.constData(), n);
            m_pending.remove(0, n);
            m_writtenBytes += n;
        }
    }
    // What can be heard now is a little behind what has been handed over.
    const qint64 audible = std::max<qint64>(0, played - qint64(m_rate) * 2 * kOutputLatencyMs / 1000);
    int heard = m_current;
    for (int i = std::max(m_current, 0); i < m_units.size(); ++i) {
        if (m_units[i].endByte == 0 || audible < m_units[i].endByte) {
            heard = i;
            break;
        }
        heard = i + 1;
    }
    if (heard >= m_units.size()) {
        finishPlayback();
        return;
    }
    if (heard != m_current)
        setPosition(heard);
    if (heard >= 0 && heard < m_units.size()) {
        const Unit &u = m_units[heard];
        const qint64 start = u.startByte;
        const qint64 spoken = u.speechEnd - u.speechStart;
        const qreal p = spoken > 0
            ? std::clamp(qreal(audible - start - u.speechStart) / qreal(spoken), 0.0, 1.0) : 0.0;
        if (std::abs(p - m_progress) >= 0.004 || (p == 0 && m_progress != 0)) {
            m_progress = p;
            emit progressChanged();
        }
    }
    requestMore();
}

void Reader::finishPlayback()
{
    m_pump.stop();
    m_pending.clear();
    closePlayer(true);
    ++m_generation;
    m_units.clear();
    m_current = -1;
    m_paragraph = -1;
    m_sentence = -1;
    emit positionChanged();
    setState(Idle);
    emit finished();
}

void Reader::pause()
{
    if (m_state != Playing || !m_player)
        return;
    m_playedBase = playedBytes();
    m_clockRunning = false;
    ::kill(m_player->processId(), SIGSTOP);
    setState(Paused);
}

void Reader::resume()
{
    if (m_state != Paused || !m_player)
        return;
    ::kill(m_player->processId(), SIGCONT);
    m_clockBase = m_clock.elapsed();
    m_clockRunning = m_writtenBytes > m_playedBase;
    setState(Playing);
}

void Reader::toggle()
{
    if (m_state == Playing)
        pause();
    else if (m_state == Paused)
        resume();
}

void Reader::stop()
{
    if (m_state == Idle)
        return;
    m_pump.stop();
    m_pending.clear();
    ++m_generation;
    closePlayer(false);
    m_units.clear();
    m_current = -1;
    m_paragraph = -1;
    m_sentence = -1;
    emit positionChanged();
    setState(Idle);
}

void Reader::jumpToParagraph(int paragraph)
{
    if (m_state == Idle)
        return;
    restartAt(firstUnitOf(std::clamp(paragraph, 0, std::max(0, m_paragraphCount - 1))));
}

void Reader::nextParagraph()
{
    if (m_state == Idle || m_current < 0)
        return;
    restartAt(firstUnitOf(m_paragraph + 1));
}

void Reader::previousParagraph()
{
    if (m_state == Idle || m_current < 0)
        return;
    // Past the first sentence: go to the start of this paragraph; otherwise the previous one.
    const int target = m_sentence > 0 ? m_paragraph : std::max(0, m_paragraph - 1);
    restartAt(firstUnitOf(target));
}

void Reader::nextSentence()
{
    if (m_state == Idle || m_current < 0)
        return;
    restartAt(m_current + 1);
}

void Reader::previousSentence()
{
    if (m_state == Idle || m_current < 0)
        return;
    restartAt(std::max(0, m_current - 1));
}
