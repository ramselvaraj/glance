#pragma once

#include <QObject>
#include <QColor>
#include <QFileSystemWatcher>
#include <QTimer>

// Omarchy theme bridge: resolves the active theme via the omarchy CLI, parses
// its colors.toml, and hot-reloads when themes change underneath us.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor darkBackground READ darkBackground NOTIFY changed)
    Q_PROPERTY(QColor darkerBackground READ darkerBackground NOTIFY changed)
    Q_PROPERTY(QColor lighterBackground READ lighterBackground NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor mutedForeground READ mutedForeground NOTIFY changed)
    Q_PROPERTY(QColor selection READ selection NOTIFY changed)
    Q_PROPERTY(QColor red READ red NOTIFY changed)
    Q_PROPERTY(QColor yellow READ yellow NOTIFY changed)
    Q_PROPERTY(QColor green READ green NOTIFY changed)
    Q_PROPERTY(QColor cyan READ cyan NOTIFY changed)
    Q_PROPERTY(QColor blue READ blue NOTIFY changed)
    Q_PROPERTY(QColor magenta READ magenta NOTIFY changed)
    Q_PROPERTY(bool darkMode READ darkMode NOTIFY changed)

public:
    explicit Theme(QObject *parent = nullptr);

    QColor accent() const { return m_p.accent; }
    QColor background() const { return m_p.background; }
    QColor darkBackground() const { return m_p.darkBackground; }
    QColor darkerBackground() const { return m_p.darkerBackground; }
    QColor lighterBackground() const { return m_p.lighterBackground; }
    QColor foreground() const { return m_p.foreground; }
    QColor mutedForeground() const { return m_p.muted; }
    QColor selection() const { return m_p.selection; }
    QColor red() const { return m_p.red; }
    QColor yellow() const { return m_p.yellow; }
    QColor green() const { return m_p.green; }
    QColor cyan() const { return m_p.cyan; }
    QColor blue() const { return m_p.blue; }
    QColor magenta() const { return m_p.magenta; }
    bool darkMode() const { return m_p.darkMode; }

signals:
    void changed();

private slots:
    void reload();       // re-resolve active theme + repaint (debounced)
    void fileChanged();  // colors.toml edited in place

private:
    struct Palette {
        QColor accent = QColor("#89b4fa");
        QColor background = QColor("#1e1e2e");
        QColor darkBackground = QColor("#161622");
        QColor darkerBackground = QColor("#101019");
        QColor lighterBackground = QColor("#313244");
        QColor foreground = QColor("#cdd6f4");
        QColor muted = QColor("#585b70");
        QColor selection = QColor("#45475a");
        QColor red = QColor("#f38ba8");
        QColor yellow = QColor("#f9e2af");
        QColor green = QColor("#a6e3a1");
        QColor cyan = QColor("#94e2d5");
        QColor blue = QColor("#89b4fa");
        QColor magenta = QColor("#f5c2e7");
        bool darkMode = true;
    };

    void resolveThemeDir(QString *outDir);
    void parseColorsToml(const QString &path, Palette *out);
    void adopt(const Palette &p);
    void scheduleReload();

    Palette m_p;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadDebounce;
    QString m_currentToml; // watched file (full path)
};
