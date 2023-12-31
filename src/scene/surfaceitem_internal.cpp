/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/surfaceitem_internal.h"
#include "composite.h"
#include "core/renderbackend.h"
#include "internalwindow.h"

#include <QOpenGLFramebufferObject>

namespace KWin
{

SurfaceItemInternal::SurfaceItemInternal(InternalWindow *window, Scene *scene, Item *parent)
    : SurfaceItem(scene, parent)
    , m_window(window)
{
    connect(window, &Window::bufferGeometryChanged,
            this, &SurfaceItemInternal::handleBufferGeometryChanged);

    setSize(window->bufferGeometry().size());

    // The device pixel ratio of the internal window is static.
    QMatrix4x4 surfaceToBufferMatrix;
    surfaceToBufferMatrix.scale(window->bufferScale());
    setSurfaceToBufferMatrix(surfaceToBufferMatrix);
}

InternalWindow *SurfaceItemInternal::window() const
{
    return m_window;
}

QVector<QRectF> SurfaceItemInternal::shape() const
{
    return {rect()};
}

std::unique_ptr<SurfacePixmap> SurfaceItemInternal::createPixmap()
{
    return std::make_unique<SurfacePixmapInternal>(this);
}

void SurfaceItemInternal::handleBufferGeometryChanged(const QRectF &old)
{
    if (m_window->bufferGeometry().size() != old.size()) {
        discardPixmap();
    }
    setSize(m_window->bufferGeometry().size());
}

SurfacePixmapInternal::SurfacePixmapInternal(SurfaceItemInternal *item, QObject *parent)
    : SurfacePixmap(Compositor::self()->backend()->createSurfaceTextureInternal(this), parent)
    , m_item(item)
{
}

QOpenGLFramebufferObject *SurfacePixmapInternal::fbo() const
{
    return m_fbo.get();
}

QImage SurfacePixmapInternal::image() const
{
    return m_rasterBuffer;
}

void SurfacePixmapInternal::create()
{
    update();
}

void SurfacePixmapInternal::update()
{
    const InternalWindow *window = m_item->window();

    if (window->fbo()) {
        m_fbo = window->fbo();
        m_size = m_fbo->size();
        m_hasAlphaChannel = true;
    } else if (!window->image().isNull()) {
        m_rasterBuffer = window->image();
        m_size = m_rasterBuffer.size();
        m_hasAlphaChannel = m_rasterBuffer.hasAlphaChannel();
    }
}

bool SurfacePixmapInternal::isValid() const
{
    return m_fbo != nullptr || !m_rasterBuffer.isNull();
}

} // namespace KWin
