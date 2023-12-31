/*
    SPDX-FileCopyrightText: 2022 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "libkwineffects/colorspace.h"
#include "libkwineffects/kwinglutils_export.h"

#include <QImage>
#include <QMatrix4x4>
#include <variant>

namespace KWin
{

class GLFramebuffer;
class GLTexture;

class KWINGLUTILS_EXPORT RenderTarget
{
public:
    explicit RenderTarget(GLFramebuffer *fbo, const ColorDescription &colorDescription = ColorDescription::sRGB);
    explicit RenderTarget(QImage *image, const ColorDescription &colorDescription = ColorDescription::sRGB);

    QSize size() const;
    QMatrix4x4 transformation() const;
    const ColorDescription &colorDescription() const;
    QRectF applyTransformation(const QRectF &rect, const QRectF &viewport) const;
    QRect applyTransformation(const QRect &rect, const QRect &viewport) const;

    QImage *image() const;
    GLFramebuffer *framebuffer() const;
    GLTexture *texture() const;

private:
    QImage *m_image = nullptr;
    GLFramebuffer *m_framebuffer = nullptr;
    QMatrix4x4 m_transformation;
    const ColorDescription m_colorDescription;
};

} // namespace KWin
