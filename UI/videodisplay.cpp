#include "videodisplay.h"

#include <QSGSimpleTextureNode>
#include <QSGTexture>

VideoDisplay::VideoDisplay(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);
    setSmooth(false);
}

void VideoDisplay::setFrame(const QImage &image) {
    if (m_frame == image) return;
    m_frame = image;
    emit frameChanged();
    update();  // trigger repaint -> updatePaintNode
}

QSGNode *VideoDisplay::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
    }

    if (m_frame.isNull()) {
        delete node;
        return nullptr;
    }

    QSGTexture *texture = window()->createTextureFromImage(m_frame);
    if (!texture) {
        delete node;
        return nullptr;
    }
    texture->setFiltering(QSGTexture::Linear);

    node->setTexture(texture);  // node takes ownership
    node->setRect(boundingRect());

    return node;
}
