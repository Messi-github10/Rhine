#pragma once

#include <QQuickItem>
#include <QImage>
#include <QtQml/qqmlregistration.h>

class VulkanDisplay : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QImage frame READ frame NOTIFY frameChanged FINAL)

public:
    explicit VulkanDisplay(QQuickItem *parent = nullptr);

    void setFrame(const QImage &image);
    QImage frame() const { return m_frame; }

signals:
    void frameChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

private:
    QImage m_frame;
};
