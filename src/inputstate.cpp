#include "inputstate.h"

#include <QEvent>
#include <QKeyEvent>

InputState::InputState(QObject *parent)
    : QObject(parent)
{
}

bool InputState::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Space && !key->isAutoRepeat())
            setSpaceHeld(event->type() == QEvent::KeyPress);
    } else if (event->type() == QEvent::ApplicationDeactivate) {
        setSpaceHeld(false);
    }
    return QObject::eventFilter(watched, event);
}

void InputState::setSpaceHeld(bool held)
{
    if (m_spaceHeld == held)
        return;
    m_spaceHeld = held;
    emit spaceHeldChanged();
}
