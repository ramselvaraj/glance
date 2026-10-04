#pragma once

#include <QObject>

class InputState : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool spaceHeld READ spaceHeld NOTIFY spaceHeldChanged)

public:
    explicit InputState(QObject *parent = nullptr);
    bool spaceHeld() const { return m_spaceHeld; }

signals:
    void spaceHeldChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setSpaceHeld(bool held);
    bool m_spaceHeld = false;
};
