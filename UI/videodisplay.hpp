#pragma once

#include <QQuickItem>
#include <QImage>
#include <QtQml/qqmlregistration.h>

class VideoDisplay : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QImage frame READ frame WRITE setFrame NOTIFY frameChanged)

public:
    explicit VideoDisplay(QQuickItem *parent = nullptr);

    QImage frame() const { return m_frame; }
    void setFrame(const QImage &image);

signals:
    void frameChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

private:
    QImage m_frame;
};
