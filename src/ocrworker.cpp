#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTextStream>

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>

#include <cstdio>

static QJsonObject recognize(tesseract::TessBaseAPI &api, const QJsonObject &request)
{
    const QString path = request.value(QStringLiteral("path")).toString();
    Pix *pix = pixRead(path.toUtf8().constData());
    if (!pix)
        return {{QStringLiteral("error"), QStringLiteral("Could not read OCR image")}};

    api.SetPageSegMode(tesseract::PSM_SPARSE_TEXT);
    api.SetImage(pix);
    QJsonArray words;
    if (api.Recognize(nullptr) == 0) {
        tesseract::ResultIterator *it = api.GetIterator();
        if (it) {
            QJsonObject word;
            QJsonArray chars;
            int block = 0;
            int paragraph = 0;
            int line = 0;
            int wordIndex = 0;
            do {
                if (it->IsAtBeginningOf(tesseract::RIL_WORD)) {
                    if (!word.isEmpty()) {
                        word.insert(QStringLiteral("chars"), chars);
                        words.append(word);
                    }
                    if (it->IsAtBeginningOf(tesseract::RIL_BLOCK)) {
                        ++block;
                        paragraph = 0;
                        line = 0;
                    }
                    if (it->IsAtBeginningOf(tesseract::RIL_PARA))
                        ++paragraph;
                    if (it->IsAtBeginningOf(tesseract::RIL_TEXTLINE)) {
                        ++line;
                        wordIndex = 0;
                    }
                    char *value = it->GetUTF8Text(tesseract::RIL_WORD);
                    int left, top, right, bottom;
                    if (value && it->BoundingBox(tesseract::RIL_WORD,
                                                  &left, &top, &right, &bottom)) {
                        word = QJsonObject{{QStringLiteral("text"), QString::fromUtf8(value).trimmed()},
                                           {QStringLiteral("x"), left},
                                           {QStringLiteral("y"), top},
                                           {QStringLiteral("w"), right - left},
                                           {QStringLiteral("h"), bottom - top},
                                           {QStringLiteral("block"), block},
                                           {QStringLiteral("paragraph"), paragraph},
                                           {QStringLiteral("line"), line},
                                           {QStringLiteral("word"), wordIndex++}};
                        chars = QJsonArray();
                    }
                    delete[] value;
                }
                char *symbol = it->GetUTF8Text(tesseract::RIL_SYMBOL);
                int sx, sy, sr, sb;
                if (!word.isEmpty() && symbol
                        && it->BoundingBox(tesseract::RIL_SYMBOL, &sx, &sy, &sr, &sb)) {
                    chars.append(QJsonObject{{QStringLiteral("text"), QString::fromUtf8(symbol)},
                                             {QStringLiteral("x"), sx},
                                             {QStringLiteral("y"), sy},
                                             {QStringLiteral("w"), sr - sx},
                                             {QStringLiteral("h"), sb - sy}});
                }
                delete[] symbol;
            } while (it->Next(tesseract::RIL_SYMBOL));
            if (!word.isEmpty()) {
                word.insert(QStringLiteral("chars"), chars);
                words.append(word);
            }
        }
    }
    api.Clear();
    pixDestroy(&pix);
    return {{QStringLiteral("id"), request.value(QStringLiteral("id"))},
            {QStringLiteral("words"), words}};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    tesseract::TessBaseAPI api;
    if (api.Init(nullptr, "eng") != 0)
        return 1;

    QTextStream input(stdin);
    QTextStream output(stdout);
    QSocketNotifier notifier(fileno(stdin), QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
        while (input.device()->bytesAvailable() || !input.atEnd()) {
            const QString line = input.readLine();
            if (line.isNull()) {
                app.quit();
                return;
            }
            QJsonParseError error;
            const QJsonDocument request = QJsonDocument::fromJson(line.toUtf8(), &error);
            if (error.error != QJsonParseError::NoError || !request.isObject())
                continue;
            output << QJsonDocument(recognize(api, request.object()))
                          .toJson(QJsonDocument::Compact)
                   << Qt::endl;
        }
    });
    return app.exec();
}
