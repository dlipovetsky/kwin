/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2016 Roman Gilg <subdiff@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef KWIN_DRM_OBJECT_CRTC_H
#define KWIN_DRM_OBJECT_CRTC_H

#include "drm_object.h"

#include <QSharedPointer>
#include <QPoint>

namespace KWin
{

class DrmBackend;
class DrmBuffer;
class DrmDumbBuffer;
class GammaRamp;
class DrmGpu;

class DrmCrtc : public DrmObject
{
public:
    DrmCrtc(DrmGpu *gpu, uint32_t crtcId, int pipeIndex);

    enum class PropertyIndex : uint32_t {
        ModeId = 0,
        Active,
        VrrEnabled,
        Gamma_LUT,
        Count
    };

    bool init() override;
    bool needsModeset() const override;

    int pipeIndex() const;
    int gammaRampSize() const;
    drmModeModeInfo queryCurrentMode();

    QSharedPointer<DrmBuffer> current() const;
    QSharedPointer<DrmBuffer> next() const;
    void setCurrent(const QSharedPointer<DrmBuffer> &buffer);
    void setNext(const QSharedPointer<DrmBuffer> &buffer);
    void flipBuffer();

    bool setLegacyCursor(const QSharedPointer<DrmDumbBuffer> buffer, const QPoint &hotspot);
    bool moveLegacyCursor(const QPoint &pos);
    void setLegacyCursor();
    bool isCursorVisible(const QRect &output) const;
    QPoint cursorPos() const;

private:
    DrmScopedPointer<drmModeCrtc> m_crtc;
    QSharedPointer<DrmBuffer> m_currentBuffer;
    QSharedPointer<DrmBuffer> m_nextBuffer;
    int m_pipeIndex;

    struct {
        QPoint pos;
        QPoint hotspot;
        QSharedPointer<DrmDumbBuffer> buffer;
        bool posDirty = true;
        bool bufferDirty = true;
    } m_cursor;
};

}

#endif

