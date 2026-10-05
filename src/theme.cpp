#include "theme.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QDebug>

Theme::Theme(QObject *parent)
    : QObject(parent)
{
    m_reloadDebounce.setSingleShot(true);
    m_reloadDebounce.setInterval(200);
    connect(&m_reloadDebounce, &QTimer::timeout, this, &Theme::reload);

    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &Theme::scheduleReload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &Theme::scheduleReload);

    reload();
}

void Theme::resolveThemeDir(QString *outDir)
{
    outDir->clear();

    QProcess current;
    current.start(QStringLiteral("omarchy"),
                  {QStringLiteral("theme"), QStringLiteral("current")});
    if (current.waitForFinished(3000) && current.exitStatus() == QProcess::NormalExit) {
        const QString themeName =
            QString::fromUtf8(current.readAllStandardOutput()).trimmed();
        if (!themeName.isEmpty()) {
            // Candidates, in order of preference: dir spec'd by the omarchy CLI
            // (exact case), then user + stock dirs (case-insensitive; user
            // themes are lowercased on disk but reported cased).
            const QString home = QDir::homePath();
            // On-disk dir names are lowercase and hyphenated ("City From
            // Above Warm" -> "city-from-above-warm").
            const QString slug = themeName.toLower().simplified()
                                     .replace(QLatin1Char(' '), QLatin1Char('-'));
            QList<QString> dirs;
            QProcess dir;
            dir.start(QStringLiteral("omarchy"),
                      {QStringLiteral("theme"), QStringLiteral("dir"), themeName});
            if (dir.waitForFinished(3000) && dir.exitStatus() == QProcess::NormalExit) {
                const QString out =
                    QString::fromUtf8(dir.readAllStandardOutput()).trimmed();
                if (!out.isEmpty())
                    dirs.append(out);
            }
            dirs << home + QStringLiteral("/.config/omarchy/themes/") + slug
                 << QStringLiteral("/usr/share/omarchy/themes/") + slug;

            for (const QString &d : dirs) {
                if (!d.isEmpty() && QFile::exists(d + QStringLiteral("/colors.toml"))) {
                    *outDir = d;
                    return;
                }
            }
        }
    }

    if (outDir->isEmpty())
        *outDir = QStringLiteral("/usr/share/omarchy/themes/catppuccin");
}

void Theme::parseColorsToml(const QString &path, Palette *out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    while (!f.atEnd()) {
        QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(u'#') || line.startsWith(u'['))
            continue;

        const int eq = line.indexOf(u'=');
        if (eq < 1)
            continue;

        const QString key = line.left(eq).trimmed().toLower();

        QString value = line.mid(eq + 1).trimmed();
        if (value.startsWith(u'"')) {
            const int close = value.indexOf(u'"', 1);
            value = close > 0 ? value.mid(1, close - 1) : value.mid(1);
        } else {
            const int hash = value.indexOf(u'#');
            if (hash > 0)
                value = value.left(hash).trimmed();
        }
        if (value.isEmpty() || value == u"\"\"")
            continue;

        // Bare hex values need the # for QColor
        if (value.size() == 6) {
            bool allHex = true;
            for (const QChar c : value)
                if (!c.isLetterOrNumber() ||
                    (c.toLower() > u'f' && c.toLower() <= u'z'))
                    allHex = false;
            if (allHex)
                value = u'#' + value;
        }

        if (key == QStringLiteral("mode")) {
            out->darkMode =
                !(value.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0);
        } else if (key == QStringLiteral("accent")) {
            out->accent = QColor(value);
        } else if (key == QStringLiteral("background")) {
            out->background = QColor(value);
        } else if (key == QStringLiteral("dark_background") ||
            key == QStringLiteral("dark_bg")) {
            out->darkBackground = QColor(value);
        } else if (key == QStringLiteral("darker_background") ||
            key == QStringLiteral("darker_bg")) {
            out->darkerBackground = QColor(value);
        } else if (key == QStringLiteral("lighter_background") ||
            key == QStringLiteral("lighter_bg")) {
            out->lighterBackground = QColor(value);
        } else if (key == QStringLiteral("foreground") ||
                   key == QStringLiteral("bright_foreground")) {
            out->foreground = QColor(value);
        } else if (key == QStringLiteral("muted") ||
                   key == QStringLiteral("dark_foreground")) {
            out->muted = QColor(value);
        } else if (key == QStringLiteral("selection")) {
            out->selection = QColor(value);
        } else if (key == QStringLiteral("red")) {
            out->red = QColor(value);
        } else if (key == QStringLiteral("yellow")) {
            out->yellow = QColor(value);
        } else if (key == QStringLiteral("green")) {
            out->green = QColor(value);
        } else if (key == QStringLiteral("cyan")) {
            out->cyan = QColor(value);
        } else if (key == QStringLiteral("blue")) {
            out->blue = QColor(value);
        } else if (key == QStringLiteral("magenta")) {
            out->magenta = QColor(value);
        }
    }
}

void Theme::adopt(const Palette &p)
{
    m_p = p;
    emit changed();
}

void Theme::reload()
{
    QString dir;
    resolveThemeDir(&dir);
    if (dir.isEmpty()) {
        qWarning() << "glance: no usable omarchy theme dir found";
        return;
    }

    Palette p;
    const QString toml = dir + QStringLiteral("/colors.toml");
    parseColorsToml(toml, &p);
    adopt(p);

    // Re-point watchers at whatever is active now.
    m_currentToml = toml;
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    m_watcher.addPath(toml);

    QStringList dirs = m_watcher.directories();
    for (const QString &d :
         {QStringLiteral("/usr/share/omarchy/themes"),
          QDir::homePath() + QStringLiteral("/.config/omarchy/themes")}) {
        if (!dirs.contains(d) && QFile::exists(d))
            m_watcher.addPath(d);
    }
}

void Theme::fileChanged()
{
    // In-place edits can arrive as delete+recreate; debounce and re-resolve
    // fully so we also re-add the (now removed) watch on the new file.
    m_reloadDebounce.start();
}

void Theme::scheduleReload()
{
    m_reloadDebounce.start();
}
