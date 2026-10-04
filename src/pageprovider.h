#pragma once

#include <QQuickAsyncImageProvider>

#include "document.h"

// image://pages/<id> where id is one of:
//   p<page>_<exp>_<dpr>   page render, texture scale = pow(1.25, exp) * dpr
//   t<page>_<dpr>         thumbnail render at width 140 * dpr
class PageProvider : public QQuickAsyncImageProvider {
public:
    explicit PageProvider(Document *doc, QObject *parent = nullptr)
        : QQuickAsyncImageProvider(), m_doc(doc)
    {
        setParent(parent);
    }

    QQuickImageResponse *requestImageResponse(const QString &id,
                                              const QSize &requestedSize) override;

private:
    Document *m_doc;
};
