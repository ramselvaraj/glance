#include "features.h"

#include <QSettings>

Features::Features(QObject *parent)
    : QObject(parent)
{
    QSettings settings;
    m_ai = settings.value(QStringLiteral("features/ai"), false).toBool();
}

void Features::setAi(bool on)
{
    if (on == m_ai)
        return;
    m_ai = on;
    QSettings settings;
    settings.setValue(QStringLiteral("features/ai"), on);
    emit aiChanged();
}
