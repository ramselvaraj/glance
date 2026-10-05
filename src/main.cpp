#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCommandLineParser>
#include <QFile>
#include <QTimer>
#include <QElapsedTimer>
#include <QCursor>
#include <QImageReader>
#include <QScreen>
#include <cstdio>
#include <algorithm>

#include "document.h"
#include "pageprovider.h"
#include "ocrmanager.h"
#include "inputstate.h"
#include "theme.h"

static void stderrMessageHandler(QtMsgType, const QMessageLogContext &,
                                 const QString &msg)
{
    fprintf(stderr, "%s\n", msg.toUtf8().constData());
}

int main(int argc, char *argv[])
{
    qInstallMessageHandler(stderrMessageHandler);

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("glance");
    QGuiApplication::setDesktopFileName("glance");
    QGuiApplication::setOrganizationName("glance");

    QCommandLineParser parser;
    parser.setApplicationDescription("glance — macOS-Preview-style PDF and image viewer");
    parser.addHelpOption();
    QCommandLineOption perfOpt(QStringLiteral("perf"),
                               QStringLiteral("Run the built-in performance probe on startup"));
    parser.addOption(perfOpt);
    QCommandLineOption selfOpt(QStringLiteral("selftest"),
                               QStringLiteral("Print outline/search diagnostics and exit"));
    parser.addOption(selfOpt);
    QCommandLineOption ocrSelfOpt(QStringLiteral("ocr-selftest"),
                                   QStringLiteral("Recognize a page and exit"),
                                   QStringLiteral("page"), QStringLiteral("1"));
    parser.addOption(ocrSelfOpt);
    QCommandLineOption ocrCancelSelfOpt(QStringLiteral("ocr-cancel-selftest"),
                                        QStringLiteral("Cancel and restart page recognition"),
                                        QStringLiteral("page"), QStringLiteral("1"));
    parser.addOption(ocrCancelSelfOpt);
    QCommandLineOption selectSelfOpt(QStringLiteral("select-selftest"),
                                     QStringLiteral("Select text between two points: page,x1,y1,x2,y2"),
                                     QStringLiteral("spec"));
    parser.addOption(selectSelfOpt);
    parser.addPositionalArgument("file", "Document to open (pdf/images). Optional.");
    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    const QString filePath = positional.isEmpty() ? QString()
                                                  : positional.first();

    int initialWidth = 1000;
    int initialHeight = 720;
    bool initialIsImage = false;
    if (!filePath.isEmpty()
            && QFileInfo(filePath).suffix().compare(QStringLiteral("pdf"),
                                                    Qt::CaseInsensitive) != 0) {
        QImageReader reader(filePath);
        const QSize pixels = reader.size();
        if (pixels.isValid()) {
            initialIsImage = true;
            QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            const QRect available = screen ? screen->availableGeometry()
                                           : QRect(0, 0, 1280, 800);
            const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
            const qreal naturalWidth = pixels.width() / dpr;
            const qreal naturalHeight = pixels.height() / dpr;
            const qreal maxWidth = available.width() * 0.88;
            const qreal maxHeight = available.height() * 0.88;
            const qreal scale = std::min({1.0,
                                          (maxWidth - 24) / naturalWidth,
                                          (maxHeight - 68) / naturalHeight});
            initialWidth = qMax(320, qRound(naturalWidth * scale + 24));
            initialHeight = qMax(220, qRound(naturalHeight * scale + 68));
        }
    }

    auto *doc = new Document(filePath, &app);
    if (!filePath.isEmpty() && !doc->isOpen()) {
        fprintf(stderr, "glance: could not open '%s'\n", filePath.toUtf8().constData());
        return 1;
    }

    Theme theme;
    OcrManager ocr;
    InputState input;
    app.installEventFilter(&input);

    if (parser.isSet(ocrSelfOpt) || parser.isSet(ocrCancelSelfOpt)) {
        if (filePath.isEmpty() || !doc->isOpen())
            return 1;
        const bool cancelTest = parser.isSet(ocrCancelSelfOpt);
        if (cancelTest)
            ocr.setCacheEnabled(false);
        bool pageOk = false;
        const int ocrPage = parser.value(cancelTest ? ocrCancelSelfOpt : ocrSelfOpt)
            .toInt(&pageOk) - 1;
        if (!pageOk || ocrPage < 0 || ocrPage >= doc->pageCount())
            return 1;
        auto *timer = new QElapsedTimer;
        auto *pass = new int(0);
        timer->start();
        QObject::connect(&ocr, &OcrManager::finished, &app,
                         [&app, &ocr, doc, filePath, ocrPage, cancelTest, timer, pass](int generation, int page, const QString &text,
                                 const QVariantList &words, const QString &error) {
            if (cancelTest && generation != 2)
                return;
            fprintf(stderr, "ocr-selftest: pass=%d page=%d chars=%d words=%d ms=%lld error='%s'\n",
                    *pass + 1,
                    page + 1, int(text.size()), int(words.size()),
                    static_cast<long long>(timer->elapsed()),
                    error.toUtf8().constData());
            int characterCount = 0;
            bool charactersMatch = true;
            for (const QVariant &value : words) {
                const QVariantMap word = value.toMap();
                QString characterText;
                for (const QVariant &character : word.value(QStringLiteral("chars")).toList())
                    characterText += character.toMap().value(QStringLiteral("text")).toString();
                characterCount += characterText.size();
                if (!characterText.isEmpty() && characterText != word.value(QStringLiteral("text")).toString())
                    charactersMatch = false;
            }
            fprintf(stderr, "ocr-selftest: character-boxes=%d\n", characterCount);
            if (!error.isEmpty() || words.isEmpty() || characterCount == 0 || !charactersMatch) {
                app.exit(2);
                return;
            }
            if (!cancelTest && (*pass)++ == 0) {
                timer->restart();
                ocr.recognize(filePath, ocrPage, doc->pageSizePt(ocrPage), 2);
                return;
            }
            app.exit(0);
        });
        QTimer::singleShot(30000, &app, [&app, &ocr] {
            ocr.cancel();
            app.exit(3);
        });
        ocr.recognize(filePath, ocrPage, doc->pageSizePt(ocrPage), 1);
        if (cancelTest) {
            QTimer::singleShot(25, &app, [&ocr, doc, filePath, ocrPage, timer] {
                ocr.cancel();
                timer->restart();
                ocr.recognize(filePath, ocrPage, doc->pageSizePt(ocrPage), 2);
            });
        }
        return app.exec();
    }

    if (parser.isSet(selectSelfOpt)) {
        const QStringList f = parser.value(selectSelfOpt).split(QLatin1Char(','));
        if (f.size() != 5 || !doc->isOpen())
            return 1;
        const QVariantMap r = doc->selectText(f[0].toInt() - 1,
            QPointF(f[1].toDouble(), f[2].toDouble()), QPointF(f[3].toDouble(), f[4].toDouble()));
        double minY = 1e9, maxY = -1e9;
        for (const QVariant &v : r.value("boxes").toList()) {
            const QVariantMap b = v.toMap();
            minY = std::min(minY, b.value("y").toDouble());
            maxY = std::max(maxY, b.value("y").toDouble() + b.value("h").toDouble());
        }
        fprintf(stderr, "select-selftest: boxes=%d y=%.1f..%.1f text=[%s]\n",
                int(r.value("boxes").toList().size()), minY, maxY,
                r.value("text").toString().toUtf8().constData());
        return 0;
    }

    if (parser.isSet(selfOpt)) {
        const QVariantList ol = doc->outline();
        fprintf(stderr, "selftest: outline entries=%d\n", int(ol.size()));
        if (!ol.isEmpty()) {
            const QVariantMap first = ol.first().toMap();
            fprintf(stderr, "selftest: first='%s' page=%d\n",
                    first.value("title").toString().toUtf8().constData(),
                    first.value("page").toInt());
        }
        const QVariantMap s = doc->searchFrom(0, QStringLiteral("the"));
        fprintf(stderr, "selftest: search 'the' -> page=%d boxes=%d\n",
                s.value("page").toInt(), int(s.value("boxes").toList().size()));
        const QVariantList allSearch = doc->searchAll(QStringLiteral("RP2040"));
        fprintf(stderr, "selftest: searchAll 'RP2040' pages=%d\n", int(allSearch.size()));
        const QSizeF size = doc->pageSizePt(0);
        fprintf(stderr, "selftest: labels page1='%s' page2='%s' page42='%s' find('40')=%d\n",
                doc->pageLabel(0).toUtf8().constData(), doc->pageLabel(1).toUtf8().constData(),
                doc->pageLabel(41).toUtf8().constData(), doc->pageForLabel(QStringLiteral("40")));
        fprintf(stderr, "selftest: page0 text=%s images=%s\n",
                doc->pageHasText(0) ? "yes" : "no",
                doc->pageHasImages(0) ? "yes" : "no");
        const QVariantMap selection = doc->selectText(
            0, QPointF(0, 0), QPointF(size.width(), size.height()));
        fprintf(stderr, "selftest: selection chars=%d boxes=%d\n",
                int(selection.value("text").toString().size()),
                int(selection.value("boxes").toList().size()));
        const QVariantList searchBoxes = s.value("boxes").toList();
        if (!searchBoxes.isEmpty()) {
            const QVariantMap box = searchBoxes.first().toMap();
            const QPointF hit(box.value("x").toDouble() + box.value("w").toDouble() / 2,
                              box.value("y").toDouble() + box.value("h").toDouble() / 2);
            const int hitPage = s.value("page").toInt();
            const QVariantMap word = doc->selectTextAt(hitPage, hit, QStringLiteral("word"));
            const QVariantMap line = doc->selectTextAt(hitPage, hit, QStringLiteral("line"));
            fprintf(stderr, "selftest: point word chars=%d line chars=%d\n",
                    int(word.value("text").toString().size()),
                    int(line.value("text").toString().size()));
            bool pointContained = false;
            for (const QVariant &value : word.value("boxes").toList()) {
                const QVariantMap selectedBox = value.toMap();
                const QRectF rect(selectedBox.value("x").toDouble(),
                                  selectedBox.value("y").toDouble(),
                                  selectedBox.value("w").toDouble(),
                                  selectedBox.value("h").toDouble());
                if (rect.adjusted(-5, -5, 5, 5).contains(hit)) {
                    pointContained = true;
                    break;
                }
            }
            fprintf(stderr, "selftest: point contained=%s hit=(%.1f,%.1f)\n",
                    pointContained ? "yes" : "no", hit.x(), hit.y());
            if (!pointContained)
                return 3;
        }
        if (doc->pageCount() > 1) {
            const QSizeF nextSize = doc->pageSizePt(1);
            const QVariantMap range = doc->selectTextRange(
                0, QPointF(0, 0), 1, QPointF(nextSize.width(), nextSize.height()));
            const QVariantMap reverseRange = doc->selectTextRange(
                1, QPointF(nextSize.width(), nextSize.height()), 0, QPointF(0, 0));
            fprintf(stderr, "selftest: range chars=%d pages=%d\n",
                    int(range.value("text").toString().size()),
                    int(range.value("pages").toList().size()));
            fprintf(stderr, "selftest: reverse range match=%s\n",
                    range.value("text") == reverseRange.value("text") ? "yes" : "no");
            if (range.value("text") != reverseRange.value("text"))
                return 2;
        }
        if (doc->pageCount() > 2) {
            const QSizeF firstTextSize = doc->pageSizePt(1);
            const QSizeF secondTextSize = doc->pageSizePt(2);
            const QVariantMap down = doc->selectTextRange(
                1, QPointF(firstTextSize.width() / 2, firstTextSize.height() / 2),
                2, QPointF(secondTextSize.width() / 2, secondTextSize.height() / 2));
            const QVariantMap up = doc->selectTextRange(
                2, QPointF(secondTextSize.width() / 2, secondTextSize.height() / 2),
                1, QPointF(firstTextSize.width() / 2, firstTextSize.height() / 2));
            fprintf(stderr, "selftest: cross-page direction match=%s down=%d up=%d\n",
                    down.value("text") == up.value("text") ? "yes" : "no",
                    int(down.value("text").toString().size()),
                    int(up.value("text").toString().size()));
        }
        return 0;
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("Doc", doc);
    engine.rootContext()->setContextProperty("Ocr", &ocr);
    engine.rootContext()->setContextProperty("Input", &input);
    engine.rootContext()->setContextProperty("InitialWindowWidth", initialWidth);
    engine.rootContext()->setContextProperty("InitialWindowHeight", initialHeight);
    engine.rootContext()->setContextProperty("InitialIsImage", initialIsImage);
    engine.addImageProvider("pages", new PageProvider(doc, doc));
    qmlRegisterSingletonInstance("glance", 1, 0, "Theme", &theme);

    QString qmlMain = QCoreApplication::applicationDirPath()
        + QStringLiteral("/qml/Main.qml");
    if (!QFile::exists(qmlMain))
        qmlMain = QString(QML_MAIN_SOURCE_S);
    engine.load(QUrl::fromLocalFile(qmlMain));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
