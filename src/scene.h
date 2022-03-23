/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef KWIN_SCENE_H
#define KWIN_SCENE_H

#include "kwineffects.h"
#include "renderlayerdelegate.h"
#include "toplevel.h"
#include "utils/common.h"

#include <QElapsedTimer>
#include <QMatrix4x4>

namespace KWin
{

namespace Decoration
{
class DecoratedClientImpl;
}

class AbstractOutput;
class DecorationRenderer;
class Deleted;
class EffectFrameImpl;
class EffectWindowImpl;
class GLTexture;
class Item;
class RenderLoop;
class Scene;
class Shadow;
class ShadowItem;
class SurfaceItem;
class SurfacePixmapInternal;
class SurfacePixmapWayland;
class SurfacePixmapX11;
class SurfaceTexture;
class WindowItem;

class SceneDelegate : public RenderLayerDelegate
{
    Q_OBJECT

public:
    explicit SceneDelegate(Scene *scene, QObject *parent = nullptr);
    explicit SceneDelegate(Scene *scene, AbstractOutput *output, QObject *parent = nullptr);
    ~SceneDelegate() override;

    QRegion repaints() const override;
    SurfaceItem *scanoutCandidate() const override;
    void prePaint() override;
    void postPaint() override;
    void paint(const QRegion &region) override;

private:
    Scene *m_scene;
    AbstractOutput *m_output = nullptr;
};

class KWIN_EXPORT Scene : public QObject
{
    Q_OBJECT

public:
    explicit Scene(QObject *parent = nullptr);
    ~Scene() override;
    class EffectFrame;
    class Window;

    void initialize();

    /**
     * Schedules a repaint for the specified @a region.
     */
    void addRepaint(const QRegion &region);
    void addRepaint(const QRect &rect);
    void addRepaint(int x, int y, int width, int height);
    void addRepaintFull();
    QRegion damage() const;

    QRect geometry() const;
    void setGeometry(const QRect &rect);

    QList<SceneDelegate *> delegates() const;
    void addDelegate(SceneDelegate *delegate);
    void removeDelegate(SceneDelegate *delegate);

    // Returns true if the ctor failed to properly initialize.
    virtual bool initFailed() const = 0;

    SurfaceItem *scanoutCandidate() const;
    void prePaint(AbstractOutput *output);
    void postPaint();
    virtual void paint(const QRegion &region) = 0;

    /**
     * Adds the Toplevel to the Scene.
     *
     * If the toplevel gets deleted, then the scene will try automatically
     * to re-bind an underlying scene window to the corresponding Deleted.
     *
     * @param toplevel The window to be added.
     * @note You can add a toplevel to scene only once.
     */
    void addToplevel(Toplevel *toplevel);

    /**
     * Removes the Toplevel from the Scene.
     *
     * @param toplevel The window to be removed.
     * @note You can remove a toplevel from the scene only once.
     */
    void removeToplevel(Toplevel *toplevel);

    /**
     * @brief Creates the Scene backend of an EffectFrame.
     *
     * @param frame The EffectFrame this Scene::EffectFrame belongs to.
     */
    virtual Scene::EffectFrame *createEffectFrame(EffectFrameImpl *frame) = 0;
    /**
     * @brief Creates the Scene specific Shadow subclass.
     *
     * An implementing class has to create a proper instance. It is not allowed to
     * return @c null.
     *
     * @param toplevel The Toplevel for which the Shadow needs to be created.
     */
    virtual Shadow *createShadow(Toplevel *toplevel) = 0;
    // Flags controlling how painting is done.
    enum {
        // Window (or at least part of it) will be painted opaque.
        PAINT_WINDOW_OPAQUE = 1 << 0,
        // Window (or at least part of it) will be painted translucent.
        PAINT_WINDOW_TRANSLUCENT = 1 << 1,
        // Window will be painted with transformed geometry.
        PAINT_WINDOW_TRANSFORMED = 1 << 2,
        // Paint only a region of the screen (can be optimized, cannot
        // be used together with TRANSFORMED flags).
        PAINT_SCREEN_REGION = 1 << 3,
        // Whole screen will be painted with transformed geometry.
        PAINT_SCREEN_TRANSFORMED = 1 << 4,
        // At least one window will be painted with transformed geometry.
        PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS = 1 << 5,
        // Clear whole background as the very first step, without optimizing it
        PAINT_SCREEN_BACKGROUND_FIRST = 1 << 6,
        // PAINT_DECORATION_ONLY = 1 << 7 has been removed
        // Window will be painted with a lanczos filter.
        PAINT_WINDOW_LANCZOS = 1 << 8
        // PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS_WITHOUT_FULL_REPAINTS = 1 << 9 has been removed
    };

    virtual bool makeOpenGLContextCurrent();
    virtual void doneOpenGLContextCurrent();
    virtual bool supportsNativeFence() const;

    virtual QMatrix4x4 screenProjectionMatrix() const;

    virtual DecorationRenderer *createDecorationRenderer(Decoration::DecoratedClientImpl *) = 0;

    /**
     * Whether the Scene is able to drive animations.
     * This is used as a hint to the effects system which effects can be supported.
     * If the Scene performs software rendering it is supposed to return @c false,
     * if rendering is hardware accelerated it should return @c true.
     */
    virtual bool animationsSupported() const = 0;

    /**
     * The QPainter used by a QPainter based compositor scene.
     * Default implementation returns @c nullptr;
     */
    virtual QPainter *scenePainter() const;

    /**
     * The render buffer used by a QPainter based compositor.
     * Default implementation returns @c nullptr.
     */
    virtual QImage *qpainterRenderBuffer(AbstractOutput *output) const;

    /**
     * The backend specific extensions (e.g. EGL/GLX extensions).
     *
     * Not the OpenGL (ES) extension!
     *
     * Default implementation returns empty list
     */
    virtual QVector<QByteArray> openGLPlatformInterfaceExtensions() const;

    virtual QSharedPointer<GLTexture> textureForOutput(AbstractOutput *output) const
    {
        Q_UNUSED(output);
        return {};
    }

    virtual SurfaceTexture *createSurfaceTextureInternal(SurfacePixmapInternal *pixmap);
    virtual SurfaceTexture *createSurfaceTextureX11(SurfacePixmapX11 *pixmap);
    virtual SurfaceTexture *createSurfaceTextureWayland(SurfacePixmapWayland *pixmap);

    virtual void paintDesktop(int desktop, int mask, const QRegion &region, ScreenPaintData &data);

    QMatrix4x4 renderTargetProjectionMatrix() const;
    QRect renderTargetRect() const;
    void setRenderTargetRect(const QRect &rect);
    qreal renderTargetScale() const;
    void setRenderTargetScale(qreal scale);

    QRegion mapToRenderTarget(const QRegion &region) const;

Q_SIGNALS:
    void frameRendered();

public Q_SLOTS:
    // a window has been closed
    void windowClosed(KWin::Toplevel *c, KWin::Deleted *deleted);

protected:
    virtual Window *createWindow(Toplevel *toplevel) = 0;
    void createStackingOrder();
    void clearStackingOrder();
    // shared implementation, starts painting the screen
    void paintScreen(const QRegion &region);
    friend class EffectsHandlerImpl;
    // called after all effects had their paintScreen() called
    void finalPaintScreen(int mask, const QRegion &region, ScreenPaintData &data);
    // shared implementation of painting the screen in the generic
    // (unoptimized) way
    void preparePaintGenericScreen();
    virtual void paintGenericScreen(int mask, const ScreenPaintData &data);
    // shared implementation of painting the screen in an optimized way
    void preparePaintSimpleScreen();
    virtual void paintSimpleScreen(int mask, const QRegion &region);
    // paint the background (not the desktop background - the whole background)
    virtual void paintBackground(const QRegion &region) = 0;
    // called after all effects had their paintWindow() called
    void finalPaintWindow(EffectWindowImpl *w, int mask, const QRegion &region, WindowPaintData &data);
    // shared implementation, starts painting the window
    virtual void paintWindow(Window *w, int mask, const QRegion &region);
    // called after all effects had their drawWindow() called
    virtual void finalDrawWindow(EffectWindowImpl *w, int mask, const QRegion &region, WindowPaintData &data);

    virtual void paintOffscreenQuickView(OffscreenQuickView *w) = 0;

    // saved data for 2nd pass of optimized screen painting
    struct Phase2Data
    {
        Window *window = nullptr;
        QRegion region;
        QRegion opaque;
        int mask = 0;
    };

    struct PaintContext
    {
        QRegion damage;
        int mask = 0;
        QVector<Phase2Data> phase2Data;
    };

    // The screen that is being currently painted
    AbstractOutput *painted_screen = nullptr;

    // windows in their stacking order
    QVector<Window *> stacking_order;

private:
    std::chrono::milliseconds m_expectedPresentTimestamp = std::chrono::milliseconds::zero();
    QList<SceneDelegate *> m_delegates;
    QHash<Toplevel *, Window *> m_windows;
    QRect m_geometry;
    QMatrix4x4 m_renderTargetProjectionMatrix;
    QRect m_renderTargetRect;
    qreal m_renderTargetScale = 1;
    // how many times finalPaintScreen() has been called
    int m_paintScreenCount = 0;
    PaintContext m_paintContext;
};

// The base class for windows representations in composite backends
class Scene::Window : public QObject
{
    Q_OBJECT

public:
    explicit Window(Toplevel *client, QObject *parent = nullptr);
    ~Window() override;
    // perform the actual painting of the window
    virtual void performPaint(int mask, const QRegion &region, const WindowPaintData &data) = 0;
    int x() const;
    int y() const;
    int width() const;
    int height() const;
    QRect geometry() const;
    QPoint pos() const;
    QSize size() const;
    QRect rect() const;
    // access to the internal window class
    // TODO eventually get rid of this
    Toplevel *window() const;
    // should the window be painted
    bool isPaintingEnabled() const;
    void resetPaintingEnabled();
    // Flags explaining why painting should be disabled
    enum {
        // Window will not be painted
        PAINT_DISABLED = 1 << 0,
        // Window will not be painted because it is deleted
        PAINT_DISABLED_BY_DELETE = 1 << 1,
        // Window will not be painted because of which desktop it's on
        PAINT_DISABLED_BY_DESKTOP = 1 << 2,
        // Window will not be painted because it is minimized
        PAINT_DISABLED_BY_MINIMIZE = 1 << 3,
        // Window will not be painted because it's not on the current activity
        PAINT_DISABLED_BY_ACTIVITY = 1 << 5
    };
    void enablePainting(int reason);
    void disablePainting(int reason);
    // is the window visible at all
    bool isVisible() const;
    // is the window fully opaque
    bool isOpaque() const;
    QRegion decorationShape() const;
    void updateToplevel(Deleted *deleted);
    void referencePreviousPixmap();
    void unreferencePreviousPixmap();
    WindowItem *windowItem() const;
    SurfaceItem *surfaceItem() const;
    ShadowItem *shadowItem() const;

protected:
    Toplevel *toplevel;

private:
    void referencePreviousPixmap_helper(SurfaceItem *item);
    void unreferencePreviousPixmap_helper(SurfaceItem *item);

    void updateWindowPosition();

    int disable_painting;
    QScopedPointer<WindowItem> m_windowItem;
    Q_DISABLE_COPY(Window)
};

class Scene::EffectFrame
{
public:
    EffectFrame(EffectFrameImpl *frame);
    virtual ~EffectFrame();
    virtual void render(const QRegion &region, double opacity, double frameOpacity) = 0;
    virtual void free() = 0;
    virtual void freeIconFrame() = 0;
    virtual void freeTextFrame() = 0;
    virtual void freeSelection() = 0;
    virtual void crossFadeIcon() = 0;
    virtual void crossFadeText() = 0;

protected:
    EffectFrameImpl *m_effectFrame;
};

inline int Scene::Window::x() const
{
    return toplevel->x();
}

inline int Scene::Window::y() const
{
    return toplevel->y();
}

inline int Scene::Window::width() const
{
    return toplevel->width();
}

inline int Scene::Window::height() const
{
    return toplevel->height();
}

inline QRect Scene::Window::geometry() const
{
    return toplevel->frameGeometry();
}

inline QSize Scene::Window::size() const
{
    return toplevel->size();
}

inline QPoint Scene::Window::pos() const
{
    return toplevel->pos();
}

inline QRect Scene::Window::rect() const
{
    return toplevel->rect();
}

inline Toplevel *Scene::Window::window() const
{
    return toplevel;
}

} // namespace

#endif
