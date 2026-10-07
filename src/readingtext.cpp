#include "readingtext.h"

#include <QHash>
#include <QSettings>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kMarginBand = 0.10;       // top/bottom fraction of the page
constexpr double kFootnoteBand = 0.65;     // footnotes sit below this fraction

enum class Kind { Body, Heading, Header, Footer, PageNum, Footnote, Caption, Table, Reference, Aside, Math };

struct Item {
    const ReadLine *line;
    int page;
    Kind kind = Kind::Body;
    bool table = false;
    QVector<double> features;
};

QString kindName(Kind k)
{
    switch (k) {
    case Kind::Heading: return QStringLiteral("heading");
    case Kind::Header: return QStringLiteral("header");
    case Kind::Footer: return QStringLiteral("footer");
    case Kind::PageNum: return QStringLiteral("pagenum");
    case Kind::Footnote: return QStringLiteral("footnote");
    case Kind::Caption: return QStringLiteral("caption");
    case Kind::Table: return QStringLiteral("table");
    case Kind::Reference: return QStringLiteral("reference");
    case Kind::Aside: return QStringLiteral("aside");
    case Kind::Math: return QStringLiteral("math");
    default: return QStringLiteral("body");
    }
}

// Same text with digits folded, so "Page 3" and "Page 4" compare equal.
QString signature(const QString &text)
{
    QString s = text.simplified().toLower();
    for (QChar &c : s)
        if (c.isDigit())
            c = QLatin1Char('#');
    return s;
}

bool isPageNumber(const QString &text)
{
    static const QRegularExpression re(
        QStringLiteral(R"re(^[\s\-–—\[(]*(page|p\.|pg\.)?\s*(\d{1,4}|[ivxlc]{1,7})(\s*(of|/)\s*\d{1,4})?[\s\-–—\])]*$)re"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return re.match(text.normalized(QString::NormalizationForm_KC).trimmed()).hasMatch();
}

bool startsWithBullet(const QString &t)
{
    const QChar c = t.trimmed().isEmpty() ? QChar() : t.trimmed().front();
    return c == QChar(0x2022) || c == QChar(0x25AA) || c == QChar(0x25E6) || c == QChar(0x2013)
        || c == QChar(0x2014) || c == QLatin1Char('-') || c == QLatin1Char('*') || c == QChar(0x25CF);
}

bool endsSentence(const QString &t)
{
    if (t.isEmpty())
        return false;
    const QChar c = t.back();
    return c == QLatin1Char('.') || c == QLatin1Char('!') || c == QLatin1Char('?')
        || c == QLatin1Char(':') || c == QChar(0x201D) || c == QLatin1Char('"')
        || c == QLatin1Char(')') || c == QChar(0x2026);
}

double histogramMedian(const QHash<int, int> &hist)
{
    int total = 0;
    for (auto it = hist.cbegin(); it != hist.cend(); ++it)
        total += it.value();
    if (total == 0)
        return 0;
    QList<int> sizes = hist.keys();
    std::sort(sizes.begin(), sizes.end());
    int acc = 0;
    for (int s : sizes) {
        acc += hist.value(s);
        if (acc * 2 >= total)
            return s / 10.0;
    }
    return sizes.last() / 10.0;
}

// Typical text size, weighted by characters; sizes in tenths of a point.
double medianBodySize(const QVector<QVector<ReadLine>> &pages)
{
    QHash<int, int> hist;
    for (const auto &page : pages)
        for (const ReadLine &l : page)
            if (l.size > 0)
                hist[int(std::lround(l.size * 10))] += l.text.size();
    return histogramMedian(hist);
}

// The page's own typical size, or 0 when it has too little text to say. Slides
// and mixed layouts differ from the document at large.
double pageBodySize(const QVector<ReadLine> &page)
{
    QHash<int, int> hist;
    int chars = 0;
    for (const ReadLine &l : page)
        if (l.size > 0) {
            hist[int(std::lround(l.size * 10))] += l.text.size();
            chars += l.text.size();
        }
    return chars >= 120 ? histogramMedian(hist) : 0.0;
}

bool looksLikeCaption(const QString &t)
{
    static const QRegularExpression re(
        QStringLiteral(R"(^(figure|fig\.?|table|chart|listing|algorithm|scheme|plate|exhibit)\s+[\dIVX]+)"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(t.trimmed()).hasMatch();
}

bool looksLikeReferencesHeading(const QString &t)
{
    static const QRegularExpression re(
        QStringLiteral(R"(^(\d+\.?\s*|[ivx]+\.?\s*)?(references|bibliography|works cited|literature cited|citations)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(t.trimmed()).hasMatch();
}

bool looksLikeBackMatterHeading(const QString &t)
{
    static const QRegularExpression re(
        QStringLiteral(R"(^(\d+\.?\s*|[ivx]+\.?\s*)?(appendix|appendices|supplementary|acknowledg))"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(t.trimmed()).hasMatch();
}

bool verticalOverlap(const QRectF &a, const QRectF &b)
{
    const double overlap = std::min(a.bottom(), b.bottom()) - std::max(a.top(), b.top());
    return overlap > 0.5 * std::min(a.height(), b.height());
}

// Rows of short cells that share a baseline: the signature of a table, as
// opposed to two columns of running text (whose lines are long).
void markTables(std::vector<Item> &items, double pageWidth)
{
    const auto isCell = [&](const Item &it) {
        return it.line->text.size() <= 28 && it.line->box.width() < pageWidth * 0.3;
    };
    // A cell sharing its row with at least two others. Running text in two columns
    // has long lines, and a pair of short side-by-side lines is as likely to be
    // inline code or a slide as a table, so two peers are required.
    std::vector<bool> rowish(items.size(), false);
    for (size_t i = 0; i < items.size(); ++i) {
        if (!isCell(items[i]))
            continue;
        int peers = 0;
        for (size_t j = 0; j < items.size(); ++j)
            if (j != i && isCell(items[j]) && verticalOverlap(items[i].line->box, items[j].line->box))
                ++peers;
        rowish[i] = peers >= 2;
    }
    // A table needs at least two such rows; an isolated one is just a short line.
    for (size_t i = 0; i < items.size(); ++i) {
        if (!rowish[i])
            continue;
        bool hasOtherRow = false;
        for (size_t j = 0; j < items.size() && !hasOtherRow; ++j)
            if (j != i && rowish[j] && !verticalOverlap(items[i].line->box, items[j].line->box)
                && std::abs(items[j].line->box.center().y() - items[i].line->box.center().y())
                       < items[i].line->size * 6)
                hasOtherRow = true;
        items[i].table = hasOtherRow;
    }
}

// ---------- learned main-text model ----------

struct Interval { double a, b; };

// Horizontal extents of the main text on a page: merged spans of the long,
// body-sized lines. Several spans means several columns.
std::vector<Interval> bodyColumns(const QVector<ReadLine> &lines, double body, int minChars)
{
    std::vector<Interval> spans;
    for (const ReadLine &l : lines)
        if (body > 0 && std::abs(l.size - body) < body * 0.12 && l.text.trimmed().size() >= minChars)
            spans.push_back({l.box.left(), l.box.right()});
    std::sort(spans.begin(), spans.end(), [](const Interval &x, const Interval &y) { return x.a < y.a; });
    std::vector<Interval> merged;
    std::vector<int> support;
    for (const Interval &s : spans) {
        if (!merged.empty() && s.a <= merged.back().b) {
            merged.back().b = std::max(merged.back().b, s.b);
            ++support.back();
        } else {
            merged.push_back(s);
            support.push_back(1);
        }
    }
    std::vector<Interval> out;
    for (size_t i = 0; i < merged.size(); ++i)
        if (support[i] >= 2)   // one stray long line is not a column
            out.push_back(merged[i]);
    return out;
}

QVector<double> lineFeatures(const ReadLine &l, const QVector<ReadLine> &page, QSizeF pageSize,
                             double body, const std::vector<Interval> &columns, double repeatFraction,
                             bool pageTagged)
{
    QVector<double> f(ReadModel::kFeatures, 0.0);
    const QString t = l.text.trimmed();
    const double ref = std::max(body, 1.0);
    f[0] = 1.0;

    double distance = 0, overlapFraction = 1;
    if (!columns.empty()) {
        double best = 1e18;
        double bestOverlap = 0;
        for (const Interval &c : columns) {
            const double overlap = std::min(c.b, l.box.right()) - std::max(c.a, l.box.left());
            if (overlap > 0) {
                best = 0;
                bestOverlap = std::max(bestOverlap, overlap);
            } else {
                best = std::min(best, -overlap);
            }
        }
        distance = best;
        overlapFraction = std::min(1.0, bestOverlap / std::max(1.0, l.box.width()));
    }
    f[1] = std::min(1.0, distance / (3 * ref));
    f[2] = 1.0 - overlapFraction;

    // Vertical distance to the nearest line above or below it in the same horizontal band.
    double nearest = 1e18;
    for (const ReadLine &o : page) {
        if (&o == &l)
            continue;
        const double overlap = std::min(l.box.right(), o.box.right()) - std::max(l.box.left(), o.box.left());
        if (overlap < 0.3 * std::min(l.box.width(), o.box.width()))
            continue;
        const double gap = std::max(0.0, std::max(l.box.top(), o.box.top())
                                              - std::min(l.box.bottom(), o.box.bottom()));
        nearest = std::min(nearest, gap);
    }
    f[3] = nearest > 1e17 ? 1.0 : std::min(1.0, nearest / (4 * ref));

    f[4] = 1.0 - std::min(1.0, t.size() / 40.0);

    int letters = 0, visible = 0;
    for (const QChar ch : t) {
        if (ch.isSpace())
            continue;
        ++visible;
        if (ch.isLetter())
            ++letters;
    }
    f[5] = visible > 0 ? 1.0 - double(letters) / visible : 1.0;

    // Only text smaller than the body counts; larger type is usually a heading.
    f[6] = body > 0 ? std::min(1.0, std::max(0.0, 1.0 - l.size / body) * 2) : 0.0;

    const double w = std::max(1.0, pageSize.width());
    const double edge = std::min(l.box.left(), w - l.box.right()) / w;
    f[7] = 1.0 - std::min(1.0, std::max(0.0, edge) * 5);
    f[8] = repeatFraction;
    // In a tagged PDF whatever is outside the structure tree is page furniture.
    f[9] = (pageTagged && !l.tagged) ? 1.0 : 0.0;
    // Only text that lies wholly on a picture is a figure label; overlays and
    // backgrounds touch pictures partially.
    f[10] = l.imageFraction >= 0.9 ? 1.0 : 0.0;
    return f;
}

} // namespace

ReadModel::ReadModel()
{
    reset();
}

void ReadModel::reset()
{
    // Priors, in log-odds of "not main text": a line far outside the body
    // column, alone, short and mostly non-letters is almost surely a margin note.
    m_prior = {-3.0, 3.0, 2.0, 0.8, 1.0, 1.5, 0.6, 1.0, 1.5, 1.2, 2.5};
    m_w = m_prior;
}

ReadModel ReadModel::load()
{
    ReadModel m;
    QSettings settings;
    const QVariantList stored = settings.value(QStringLiteral("reading/weights")).toList();
    if (stored.size() == kFeatures)
        for (int i = 1; i < kFeatures; ++i)   // the bias is never stored or learned
            m.m_w[i] = std::clamp(stored[i].toDouble(), m.m_prior[i] - 1.0, m.m_prior[i] + 1.0);
    return m;
}

void ReadModel::save() const
{
    QVariantList stored;
    for (double v : m_w)
        stored.append(v);
    QSettings settings;
    settings.setValue(QStringLiteral("reading/weights"), stored);
}

double ReadModel::junkProbability(const QVector<double> &f) const
{
    double z = 0;
    for (int i = 0; i < kFeatures && i < f.size(); ++i)
        z += m_w[i] * f[i];
    return 1.0 / (1.0 + std::exp(-z));
}

void ReadModel::learn(const QVector<double> &f, bool junk)
{
    // Gentle on purpose: a correction should refine how much each cue matters,
    // never shift the whole model. The bias (index 0) is fixed, and every
    // weight stays within a band around its prior.
    const double error = (junk ? 1.0 : 0.0) - junkProbability(f);
    for (int i = 1; i < kFeatures && i < f.size(); ++i)
        m_w[i] = std::clamp(m_w[i] + 0.1 * error * f[i], m_prior[i] - 1.0, m_prior[i] + 1.0);
}


// A sentence that runs across a page break, a column break or a figure ends up
// as two paragraphs with page furniture, a caption or a table between them.
// Join the halves and leave what came between after the joined paragraph.
bool endsWithoutStop(const QString &t)
{
    const QString s = t.trimmed();
    if (s.isEmpty())
        return false;
    const QChar last = s.back();
    if (last == QLatin1Char('-') || last == QLatin1Char(',') || last == QLatin1Char(';'))
        return true;
    if (last.isLetterOrNumber()) {
        // "...of the" / "...and" cannot be the end of a sentence either way; a bare
        // noun can (headings, list items), so only a clear continuation joins.
        return true;
    }
    return false;
}

bool continuesSentence(const QString &next)
{
    const QString s = next.trimmed();
    return !s.isEmpty() && (s.front().isLower() || s.front() == QLatin1Char(')') || s.front() == QLatin1Char(','));
}

void joinBrokenParagraphs(QVector<ReadParagraph> &paras)
{
    for (int i = 0; i < paras.size(); ++i) {
        ReadParagraph &a = paras[i];
        if (a.kind != QLatin1String("body") || a.skipped || !endsWithoutStop(a.text) || a.text.size() < 30)
            continue;
        // The paragraph that carries the sentence on: the next ordinary body text,
        // with only furniture and floats allowed in between.
        int j = i + 1;
        for (; j < paras.size() && j <= i + 14; ++j) {
            const QString &k = paras[j].kind;
            if (k == QLatin1String("body"))
                break;
            if (k == QLatin1String("heading") || k == QLatin1String("reference"))
                j = paras.size();   // a new section: nothing to join
        }
        if (j >= paras.size() || j > i + 14)
            continue;
        ReadParagraph &b = paras[j];
        if (b.skipped || !continuesSentence(b.text))
            continue;
        const bool hyphen = a.text.endsWith(QLatin1Char('-')) && a.text.size() > 1
                            && a.text[a.text.size() - 2].isLetter();
        if (hyphen)
            a.text.chop(1);
        else
            a.text += QLatin1Char(' ');
        const int shift = int(a.text.size());
        for (ReadBox box : std::as_const(b.boxes)) {
            box.start += shift;
            box.end += shift;
            a.boxes.push_back(box);
        }
        a.text += b.text;
        paras.removeAt(j);
        --i;   // the joined paragraph may continue yet again
    }
}

QVector<ReadParagraph> buildReadingParagraphs(const QVector<QVector<ReadLine>> &pages,
                                              const QVector<QSizeF> &pageSizes,
                                              const ReadOptions &options,
                                              const ReadModel &model)
{
    const int pageCount = pages.size();
    const double body = medianBodySize(pages);

    // Pass 1: which margin lines repeat across pages.
    QHash<QString, int> marginCount;
    for (int p = 0; p < pageCount; ++p) {
        const double h = pageSizes.value(p).height();
        QHash<QString, bool> seen;
        for (const ReadLine &l : pages[p]) {
            const double y = l.box.center().y();
            if (y > h * kMarginBand && y < h * (1 - kMarginBand))
                continue;
            const QString sig = signature(l.text);
            if (!sig.isEmpty() && !seen.contains(sig)) {
                seen.insert(sig, true);
                ++marginCount[sig];
            }
        }
    }
    const int repeatThreshold = std::max(2, int(std::ceil(pageCount * 0.3)));

    // How many pages each line text (digits folded) appears on.
    QHash<QString, int> pageSpread;
    for (int p = 0; p < pageCount; ++p) {
        QHash<QString, bool> seen;
        for (const ReadLine &l : pages[p]) {
            const QString sig = signature(l.text);
            if (!sig.isEmpty() && !seen.contains(sig)) {
                seen.insert(sig, true);
                ++pageSpread[sig];
            }
        }
    }
    // Columns of main text, per page with the whole document as the fallback.
    QVector<QVector<ReadLine>> allLines;
    QVector<ReadLine> everything;
    for (const auto &page : pages)
        everything += page;
    std::vector<Interval> docColumns = bodyColumns(everything, body, 20);
    if (docColumns.empty())
        docColumns = bodyColumns(everything, body, 1);

    // Pass 2: classify lines.
    std::vector<std::vector<Item>> perPage(pageCount);
    bool inReferences = false;
    for (int p = 0; p < pageCount; ++p) {
        const double w = pageSizes.value(p).width();
        const double h = pageSizes.value(p).height();
        auto &items = perPage[p];
        for (const ReadLine &l : pages[p])
            if (!l.text.trimmed().isEmpty())
                items.push_back({&l, p});
        markTables(items, w);

        bool pageTagged = false;
        for (const ReadLine &l : pages[p])
            if (l.tagged) {
                pageTagged = true;
                break;
            }
        // Per page only: slides and mixed layouts have no document-wide column, and a
        // missing column must not make every line look like it is out in the margin.
        const double pageBody = pageBodySize(pages[p]);
        const double bodyP = pageBody > 0 ? pageBody : body;
        const std::vector<Interval> columns = bodyColumns(pages[p], bodyP, 25);
        for (Item &it : items) {
            const double spread = pageCount > 2
                ? std::min(1.0, double(pageSpread.value(signature(it.line->text))) / std::max(3.0, pageCount * 0.5))
                : 0.0;
            it.features = lineFeatures(*it.line, pages[p], pageSizes.value(p), bodyP, columns,
                                       spread > 0.34 ? spread : 0.0, pageTagged);
        }

        const Item *previous = nullptr;
        for (Item &it : items) {
            const ReadLine &l = *it.line;
            const QString t = l.text.trimmed();
            const double y = l.box.center().y();
            // A wrapped bullet or sentence continues the line above; it is not a heading
            // just because that text is large.
            // A caption wrapped onto a second line, still in the same block and size.
            const bool captionContinues = previous && previous->kind == Kind::Caption
                && previous->line->block == l.block
                && std::abs(previous->line->size - l.size) < 0.05 * l.size
                && !previous->line->text.trimmed().isEmpty()
                && !QStringLiteral(".!?").contains(previous->line->text.trimmed().back())
                && !it.table && !t.isEmpty()
                && (t.front().isLower() || QStringLiteral(",;:").contains(previous->line->text.trimmed().back()));
            const bool continuation = previous && previous->kind == Kind::Body
                && previous->line->block == l.block
                && std::abs(previous->line->size - l.size) < 0.05 * l.size;
            const bool top = y <= h * kMarginBand;
            const bool bottom = y >= h * (1 - kMarginBand);

            // The references heading opens the bibliography whatever the classifier
            // thinks of it (it is short and alone, like a margin note).
            if (t.size() < 40 && looksLikeReferencesHeading(t)) {
                inReferences = true;
                it.kind = Kind::Reference;
                continue;
            }

            // Display equations are told apart from inline maths by sharing no row with text.
            const bool displayFormula = l.tag == QLatin1String("formula") && [&] {
                for (const Item &o : items)
                    if (o.line != it.line && o.line->tag != QLatin1String("formula")
                        && verticalOverlap(o.line->box, l.box))
                        return false;
                return true;
            }();

            // What the PDF says about its own content outranks any guess.
            if (l.tag == QLatin1String("artifact")) {
                it.kind = top ? Kind::Header : (bottom ? Kind::Footer : Kind::Aside);
            } else if (l.tag == QLatin1String("figure")) {
                it.kind = Kind::Aside;
            } else if (l.tag == QLatin1String("toc")) {
                it.kind = Kind::Aside;
            } else if (displayFormula) {
                it.kind = Kind::Math;
            } else if (l.tag == QLatin1String("table") || (l.inGrid && !looksLikeCaption(t))) {
                it.kind = Kind::Table;
            } else if (l.tag == QLatin1String("caption")) {
                it.kind = Kind::Caption;
            } else if (l.tag == QLatin1String("footnote")) {
                it.kind = Kind::Footnote;
            } else if (l.tag == QLatin1String("reference")) {
                it.kind = Kind::Reference;
                inReferences = true;
            } else if (l.tag == QLatin1String("heading") && t.size() < 160) {
                it.kind = Kind::Heading;
            } else if ((top || bottom || y >= h * kFootnoteBand) && isPageNumber(t)) {
                it.kind = Kind::PageNum;
            } else if ((top || bottom) && marginCount.value(signature(t)) >= repeatThreshold) {
                it.kind = top ? Kind::Header : Kind::Footer;
            } else if (bodyP > 0 && l.size < bodyP * 0.88 && y >= h * kFootnoteBand
                       && !looksLikeCaption(t) && !startsWithBullet(t)) {
                it.kind = Kind::Footnote;
            } else if (model.junkProbability(it.features) > 0.5) {
                it.kind = Kind::Aside;   // sidebar numbers, figure labels, margin notes
            } else if (looksLikeCaption(t) || captionContinues) {
                it.kind = Kind::Caption;
            } else if (it.table) {
                it.kind = Kind::Table;
            } else if (bodyP > 0 && !startsWithBullet(t)
                       && ((l.size > bodyP * 1.15 && !continuation)
                           || (l.bold && t.size() < 70 && !endsSentence(t)))
                       && t.size() < 140) {
                it.kind = Kind::Heading;
            }

            // Display maths in an untagged PDF: a line set mostly in maths fonts.
            if (it.kind == Kind::Body && l.mathFraction > 0.5)
                it.kind = Kind::Math;

            // Appendices and the like end the bibliography; everything else in it,
            // however it was classified (small type, margin-like fragments), is a reference.
            if (inReferences && it.kind == Kind::Heading && looksLikeBackMatterHeading(t)) {
                inReferences = false;
            } else if (inReferences
                       && (it.kind == Kind::Body || it.kind == Kind::Aside || it.kind == Kind::Footnote
                           || it.kind == Kind::Caption || it.kind == Kind::Table
                           || it.kind == Kind::Heading)) {
                it.kind = Kind::Reference;
            }
            previous = &it;
        }

        // A table is a region, not a set of lines: group the cells found so far,
        // then everything that lies inside their bounds belongs to the table too
        // (row labels, header cells, stray numbers).
        {
            // Columns of a table sit far apart; rows sit close together.
            const double gapH = std::max(1.5 * bodyP, 0.3 * w);
            const double gapV = 1.8 * std::max(bodyP, 8.0);
            struct Cluster { QRectF rect; int cells = 0; };
            std::vector<Cluster> clusters;
            for (const Item &it : items) {
                if (it.kind != Kind::Table)
                    continue;
                bool placed = false;
                for (Cluster &cl : clusters) {
                    if (cl.rect.adjusted(-gapH, -gapV, gapH, gapV).intersects(it.line->box)) {
                        cl.rect = cl.rect.united(it.line->box);
                        ++cl.cells;
                        placed = true;
                        break;
                    }
                }
                if (!placed)
                    clusters.push_back({it.line->box, 1});
            }
            // Clusters that grew towards each other become one.
            for (bool merged = true; merged;) {
                merged = false;
                for (size_t a = 0; a < clusters.size() && !merged; ++a)
                    for (size_t b = a + 1; b < clusters.size() && !merged; ++b)
                        if (clusters[a].rect.adjusted(-gapH, -gapV, gapH, gapV).intersects(clusters[b].rect)) {
                            clusters[a].rect = clusters[a].rect.united(clusters[b].rect);
                            clusters[a].cells += clusters[b].cells;
                            clusters.erase(clusters.begin() + b);
                            merged = true;
                        }
            }
            if (qEnvironmentVariableIsSet("GLANCE_READ_TAGS"))
                for (const Cluster &cl : clusters)
                    fprintf(stderr, "table page %d cells %d rect %.0f,%.0f %.0fx%.0f\n", p, cl.cells,
                            cl.rect.x(), cl.rect.y(), cl.rect.width(), cl.rect.height());
            for (Item &it : items) {
                if (it.kind != Kind::Body && it.kind != Kind::Aside && it.kind != Kind::Heading
                    && it.kind != Kind::Footnote)
                    continue;
                for (const Cluster &cl : clusters)
                    if (cl.cells >= 3 && cl.rect.adjusted(-2, -2, 2, 2).contains(it.line->box.center())) {
                        it.kind = Kind::Table;
                        break;
                    }
            }
        }
    }

    const auto skipped = [&](Kind k) {
        switch (k) {
        case Kind::Header: case Kind::Footer: case Kind::PageNum: case Kind::Aside:
            return options.skipHeadersFooters;
        case Kind::Footnote: return options.skipFootnotes;
        case Kind::Caption: return options.skipCaptions;
        case Kind::Table: return options.skipTables;
        case Kind::Reference: return options.skipReferences;
        case Kind::Math: return options.skipMath;
        default: return false;
        }
    };

    // Pass 3: group lines into paragraphs, in document order.
    QVector<ReadParagraph> out;
    ReadParagraph cur;
    Kind curKind = Kind::Body;
    const ReadLine *prev = nullptr;
    int prevPage = -1;
    bool open = false;
    int curFeatureLines = 0;

    const auto flush = [&] {
        if (!open)
            return;
        // Text is built already normalized, so box offsets stay valid.
        if (!cur.boxes.isEmpty())
            cur.boxes.last().end = cur.text.size();
        if (!cur.text.isEmpty()) {
            if (curFeatureLines > 0)
                for (double &v : cur.features)
                    v /= curFeatureLines;
            cur.kind = kindName(curKind);
            cur.skipped = skipped(curKind);
            out.push_back(cur);
        }
        cur = ReadParagraph();
        curFeatureLines = 0;
        open = false;
        prev = nullptr;
    };

    for (int p = 0; p < pageCount; ++p) {
        for (const Item &it : perPage[p]) {
            const ReadLine &l = *it.line;
            bool startNew = !open || it.kind != curKind;
            if (!startNew && prev) {
                const bool samePage = prevPage == p;
                const double lineH = std::max(prev->size, 1.0);
                const double gap = l.box.top() - prev->box.bottom();
                const bool ended = endsSentence(prev->text.trimmed());
                const bool sizeJump = std::abs(l.size - prev->size) > prev->size * 0.15;
                if (curKind == Kind::Heading || curKind == Kind::Table) {
                    // Headings stay one paragraph per block; table rows read as a unit per block.
                    startNew = l.block != prev->block || !samePage;
                } else if (samePage) {
                    const bool newBlock = l.block != prev->block;
                    const bool bigGap = gap > lineH * 0.7;
                    const bool indented = l.box.left() - prev->box.left() > lineH * 0.8
                                          && std::abs(l.box.top() - prev->box.top()) > 0;
                    startNew = sizeJump || (ended && (newBlock || bigGap || indented))
                               || (!ended && bigGap && newBlock && gap > lineH * 1.5);
                } else {
                    // Across a page break only a finished sentence ends the paragraph.
                    startNew = ended || sizeJump;
                }
            }
            if (startNew) {
                flush();
                cur.page = p;
                curKind = it.kind;
                open = true;
            }
            // Join lines, repairing a hyphenated line break.
            QString t = l.text.normalized(QString::NormalizationForm_KC).simplified();
            if (!cur.text.isEmpty()) {
                if (cur.text.endsWith(QLatin1Char('-')) && cur.text.size() > 1
                    && cur.text[cur.text.size() - 2].isLetter() && !t.isEmpty()
                    && t.front().isLower())
                    cur.text.chop(1);
                else
                    cur.text += QLatin1Char(' ');
            }
            if (!cur.boxes.isEmpty())
                cur.boxes.last().end = cur.text.size();
            if (cur.features.isEmpty())
                cur.features = QVector<double>(ReadModel::kFeatures, 0.0);
            for (int k = 0; k < ReadModel::kFeatures && k < it.features.size(); ++k)
                cur.features[k] += it.features[k];
            ++curFeatureLines;
            cur.boxes.push_back({p, l.box, int(cur.text.size()), 0, l.cx0, l.cx1});
            cur.text += t;
            prev = &l;
            prevPage = p;
        }
    }
    flush();
    joinBrokenParagraphs(out);
    return out;
}
