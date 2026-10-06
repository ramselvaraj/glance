#pragma once

#include <QObject>

// Switches for optional capabilities. "AI features" covers everything that runs
// a model: read aloud (voice synthesis), the lyrics and magnifier views that
// belong to it, and the learned text filter. Off by default; when off, none of
// it is shown or started, and nothing is downloaded.
class Features : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ai READ ai WRITE setAi NOTIFY aiChanged)

public:
    explicit Features(QObject *parent = nullptr);

    bool ai() const { return m_ai; }
    void setAi(bool on);

signals:
    void aiChanged();

private:
    bool m_ai = false;
};
