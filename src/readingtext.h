#pragma once

#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

// Turns per-page text lines into reading paragraphs for read-aloud, and
// classifies the ones nobody wants spoken (headers, footers, page numbers,
// footnotes, captions, tables, references). Pure Qt: no MuPDF or QML types, so
// native text and OCR lines can feed the same code.

struct ReadLine {
    QRectF box;          // page coordinates, points
    double size = 0;     // font size, points
    bool bold = false;
    int block = 0;       // source text-block id; lines of one block belong together
    QString text;
    QVector<float> cx0;  // left and right edge of each character of `text`, page points
    QVector<float> cx1;
    // What the PDF itself says about this text (tagged PDFs): artifact, figure,
    // formula, table, caption, footnote, reference, toc, heading, or empty.
    QString tag;
    bool tagged = false;          // it sits inside the document's structure tree
    double mathFraction = 0;      // share of its characters set in maths fonts
    double imageFraction = 0;     // share of its box lying over a picture
    bool inGrid = false;          // inside a table as MuPDF's table finder sees it
};

struct ReadOptions {
    bool skipHeadersFooters = true;   // running headers/footers and page numbers
    bool skipFootnotes = true;
    bool skipCaptions = false;
    bool skipTables = false;
    bool skipReferences = false;
    bool skipMath = true;             // display equations
};

// Learned judgement of whether a line is main text. Each line is described by
// how it sits relative to its document's own layout (distance from the body
// column, isolation, size, shortness, repetition...), so there are no
// per-publisher rules. The weights start from sensible priors and are nudged by
// the user's corrections, then persisted.
class ReadModel {
public:
    static constexpr int kFeatures = 11;
    static ReadModel load();
    void save() const;
    // Probability that the line is NOT main text (margin note, sidebar number, figure label...).
    double junkProbability(const QVector<double> &features) const;
    // One gradient step towards `junk` for a line with these features.
    void learn(const QVector<double> &features, bool junk);
    void reset();

private:
    ReadModel();
    QVector<double> m_w;
    QVector<double> m_prior;
};

struct ReadBox {
    int page = 0;
    QRectF rect;
    int start = 0;                // [start, end) is this line's span in the paragraph text
    int end = 0;
    QVector<float> cx0;           // per-character horizontal extent of the line's text
    QVector<float> cx1;
};

struct ReadParagraph {
    int page = 0;                 // page the paragraph starts on
    QString text;
    // body, heading, header, footer, pagenum, footnote, caption, table, reference, math
    QString kind;                 // also: aside = margin or sidebar text
    bool skipped = false;         // kind is excluded by the options
    QVector<ReadBox> boxes;       // one per source line
    QVector<double> features;     // mean ReadModel features of its lines (for feedback)
};

QVector<ReadParagraph> buildReadingParagraphs(const QVector<QVector<ReadLine>> &pages,
                                              const QVector<QSizeF> &pageSizes,
                                              const ReadOptions &options,
                                              const ReadModel &model);
