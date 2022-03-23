/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "composite.h"

#include <config-kwin.h>

#include "abstract_output.h"
#include "cursorview_opengl.h"
#include "cursorview_qpainter.h"
#include "dbusinterface.h"
#include "decorations/decoratedclient.h"
#include "deleted.h"
#include "effects.h"
#include "ftrace.h"
#include "internal_client.h"
#include "openglbackend.h"
#include "outputlayer.h"
#include "overlaywindow.h"
#include "platform.h"
#include "qpainterbackend.h"
#include "renderlayer.h"
#include "renderloop.h"
#include "scene.h"
#include "scenes/opengl/scene_opengl.h"
#include "scenes/qpainter/scene_qpainter.h"
#include "screens.h"
#include "shadow.h"
#include "surfaceitem_x11.h"
#include "unmanaged.h"
#include "useractions.h"
#include "utils/common.h"
#include "utils/xcbutils.h"
#include "wayland_server.h"
#include "workspace.h"
#include "x11client.h"
#include "x11syncmanager.h"

#include <kwinglplatform.h>
#include <kwingltexture.h>

#include <KWaylandServer/surface_interface.h>

#include <KGlobalAccel>
#include <KLocalizedString>
#if KWIN_BUILD_NOTIFICATIONS
#include <KNotification>
#endif
#include <KSelectionOwner>

#include <QDateTime>
#include <QFutureWatcher>
#include <QMenu>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QTextStream>
#include <QTimerEvent>
#include <QtConcurrentRun>

#include <xcb/composite.h>
#include <xcb/damage.h>

#include <cstdio>

Q_DECLARE_METATYPE(KWin::X11Compositor::SuspendReason)

namespace KWin
{

// See main.cpp:
extern int screen_number;

extern bool is_multihead;

Compositor *Compositor::s_compositor = nullptr;
Compositor *Compositor::self()
{
    return s_compositor;
}

WaylandCompositor *WaylandCompositor::create(QObject *parent)
{
    Q_ASSERT(!s_compositor);
    auto *compositor = new WaylandCompositor(parent);
    s_compositor = compositor;
    return compositor;
}
X11Compositor *X11Compositor::create(QObject *parent)
{
    Q_ASSERT(!s_compositor);
    auto *compositor = new X11Compositor(parent);
    s_compositor = compositor;
    return compositor;
}

class CompositorSelectionOwner : public KSelectionOwner
{
    Q_OBJECT
public:
    CompositorSelectionOwner(const char *selection)
        : KSelectionOwner(selection, kwinApp()->x11Connection(), kwinApp()->x11RootWindow())
        , m_owning(false)
    {
        connect(this, &CompositorSelectionOwner::lostOwnership,
                this, [this]() {
                    m_owning = false;
                });
    }
    bool owning() const
    {
        return m_owning;
    }
    void setOwning(bool own)
    {
        m_owning = own;
    }

private:
    bool m_owning;
};

Compositor::Compositor(QObject *workspace)
    : QObject(workspace)
{
    connect(options, &Options::configChanged, this, &Compositor::configChanged);
    connect(options, &Options::animationSpeedChanged, this, &Compositor::configChanged);

    // 2 sec which should be enough to restart the compositor.
    static const int compositorLostMessageDelay = 2000;

    m_releaseSelectionTimer.setSingleShot(true);
    m_releaseSelectionTimer.setInterval(compositorLostMessageDelay);
    connect(&m_releaseSelectionTimer, &QTimer::timeout,
            this, &Compositor::releaseCompositorSelection);

    m_unusedSupportPropertyTimer.setInterval(compositorLostMessageDelay);
    m_unusedSupportPropertyTimer.setSingleShot(true);
    connect(&m_unusedSupportPropertyTimer, &QTimer::timeout,
            this, &Compositor::deleteUnusedSupportProperties);

    // Delay the call to start by one event cycle.
    // The ctor of this class is invoked from the Workspace ctor, that means before
    // Workspace is completely constructed, so calling Workspace::self() would result
    // in undefined behavior. This is fixed by using a delayed invocation.
    if (kwinApp()->platform()->isReady()) {
        QTimer::singleShot(0, this, &Compositor::start);
    }
    connect(
        kwinApp()->platform(), &Platform::readyChanged, this, [this](bool ready) {
            if (ready) {
                start();
            } else {
                stop();
            }
        },
        Qt::QueuedConnection);

    // register DBus
    new CompositorDBusInterface(this);
    FTraceLogger::create();
}

Compositor::~Compositor()
{
    deleteUnusedSupportProperties();
    destroyCompositorSelection();
    s_compositor = nullptr;
}

bool Compositor::attemptOpenGLCompositing()
{
    // Some broken drivers crash on glXQuery() so to prevent constant KWin crashes:
    if (kwinApp()->platform()->openGLCompositingIsBroken()) {
        qCWarning(KWIN_CORE) << "KWin has detected that your OpenGL library is unsafe to use";
        return false;
    }

    kwinApp()->platform()->createOpenGLSafePoint(Platform::OpenGLSafePoint::PreInit);
    auto safePointScope = qScopeGuard([]() {
        kwinApp()->platform()->createOpenGLSafePoint(Platform::OpenGLSafePoint::PostInit);
    });

    QScopedPointer<OpenGLBackend> backend(kwinApp()->platform()->createOpenGLBackend());
    if (!backend) {
        return false;
    }
    if (!backend->isFailed()) {
        backend->init();
    }
    if (backend->isFailed()) {
        return false;
    }

    QScopedPointer<Scene> scene(SceneOpenGL::createScene(backend.data(), this));
    if (!scene || scene->initFailed()) {
        return false;
    }

    m_backend = backend.take();
    m_scene = scene.take();
    m_cursorView = new OpenGLCursorView();

    // set strict binding
    if (options->isGlStrictBindingFollowsDriver()) {
        options->setGlStrictBinding(!GLPlatform::instance()->supports(LooseBinding));
    }

    qCDebug(KWIN_CORE) << "OpenGL compositing has been successfully initialized";
    return true;
}

bool Compositor::attemptQPainterCompositing()
{
    QScopedPointer<QPainterBackend> backend(kwinApp()->platform()->createQPainterBackend());
    if (!backend || backend->isFailed()) {
        return false;
    }

    QScopedPointer<Scene> scene(SceneQPainter::createScene(backend.data(), this));
    if (!scene || scene->initFailed()) {
        return false;
    }

    m_backend = backend.take();
    m_scene = scene.take();
    m_cursorView = new QPainterCursorView();

    qCDebug(KWIN_CORE) << "QPainter compositing has been successfully initialized";
    return true;
}

bool Compositor::setupStart()
{
    if (kwinApp()->isTerminating()) {
        // Don't start while KWin is terminating. An event to restart might be lingering
        // in the event queue due to graphics reset.
        return false;
    }
    if (m_state != State::Off) {
        return false;
    }
    m_state = State::Starting;

    options->reloadCompositingSettings(true);

    initializeX11();

    // There might still be a deleted around, needs to be cleared before
    // creating the scene (BUG 333275).
    if (Workspace::self()) {
        while (!Workspace::self()->deletedList().isEmpty()) {
            Workspace::self()->deletedList().first()->discard();
        }
    }

    Q_EMIT aboutToToggleCompositing();

    auto supportedCompositors = kwinApp()->platform()->supportedCompositors();
    const auto userConfigIt = std::find(supportedCompositors.begin(), supportedCompositors.end(),
                                        options->compositingMode());

    if (userConfigIt != supportedCompositors.end()) {
        supportedCompositors.erase(userConfigIt);
        supportedCompositors.prepend(options->compositingMode());
    } else {
        qCWarning(KWIN_CORE)
            << "Configured compositor not supported by Platform. Falling back to defaults";
    }

    for (auto type : qAsConst(supportedCompositors)) {
        bool stop = false;
        switch (type) {
        case OpenGLCompositing:
            qCDebug(KWIN_CORE) << "Attempting to load the OpenGL scene";
            stop = attemptOpenGLCompositing();
            break;
        case QPainterCompositing:
            qCDebug(KWIN_CORE) << "Attempting to load the QPainter scene";
            stop = attemptQPainterCompositing();
            break;
        case NoCompositing:
            qCDebug(KWIN_CORE) << "Starting without compositing...";
            stop = true;
            break;
        }

        if (stop) {
            break;
        }
    }

    if (!m_backend) {
        m_state = State::Off;

        if (auto *con = kwinApp()->x11Connection()) {
            xcb_composite_unredirect_subwindows(con, kwinApp()->x11RootWindow(),
                                                XCB_COMPOSITE_REDIRECT_MANUAL);
        }
        if (m_selectionOwner) {
            m_selectionOwner->setOwning(false);
            m_selectionOwner->release();
        }
        if (!supportedCompositors.contains(NoCompositing)) {
            qCCritical(KWIN_CORE) << "The used windowing system requires compositing";
            qCCritical(KWIN_CORE) << "We are going to quit KWin now as it is broken";
            qApp->quit();
        }
        return false;
    }

    kwinApp()->platform()->setSelectedCompositor(m_backend->compositingType());

    if (!Workspace::self() && m_backend && m_backend->compositingType() == QPainterCompositing) {
        // Force Software QtQuick on first startup with QPainter.
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        QQuickWindow::setSceneGraphBackend(QSGRendererInterface::Software);
#else
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
#endif
    }

    Q_EMIT sceneCreated();

    return true;
}

void Compositor::initializeX11()
{
    xcb_connection_t *connection = kwinApp()->x11Connection();
    if (!connection) {
        return;
    }

    if (!m_selectionOwner) {
        char selection_name[100];
        sprintf(selection_name, "_NET_WM_CM_S%d", Application::x11ScreenNumber());
        m_selectionOwner = new CompositorSelectionOwner(selection_name);
        connect(m_selectionOwner, &CompositorSelectionOwner::lostOwnership,
                this, &Compositor::stop);
    }
    if (!m_selectionOwner->owning()) {
        // Force claim ownership.
        m_selectionOwner->claim(true);
        m_selectionOwner->setOwning(true);
    }

    xcb_composite_redirect_subwindows(connection, kwinApp()->x11RootWindow(),
                                      XCB_COMPOSITE_REDIRECT_MANUAL);
}

void Compositor::cleanupX11()
{
    delete m_selectionOwner;
    m_selectionOwner = nullptr;
}

void Compositor::startupWithWorkspace()
{
    connect(kwinApp(), &Application::x11ConnectionChanged,
            this, &Compositor::initializeX11, Qt::UniqueConnection);
    connect(kwinApp(), &Application::x11ConnectionAboutToBeDestroyed,
            this, &Compositor::cleanupX11, Qt::UniqueConnection);
    initializeX11();

    Workspace::self()->markXStackingOrderAsDirty();
    Q_ASSERT(m_scene);
    m_scene->initialize();

    const QVector<AbstractOutput *> outputs = kwinApp()->platform()->enabledOutputs();
    if (kwinApp()->operationMode() == Application::OperationModeX11) {
        auto workspaceLayer = new RenderLayer(outputs.constFirst()->renderLoop());
        workspaceLayer->setDelegate(new SceneDelegate(m_scene));
        workspaceLayer->setGeometry(workspace()->geometry());
        connect(workspace(), &Workspace::geometryChanged, workspaceLayer, [workspaceLayer]() {
            workspaceLayer->setGeometry(workspace()->geometry());
        });
        addSuperLayer(workspaceLayer);
    } else {
        for (AbstractOutput *output : outputs) {
            addOutput(output);
        }
        connect(kwinApp()->platform(), &Platform::outputEnabled, this, &Compositor::addOutput);
        connect(kwinApp()->platform(), &Platform::outputDisabled, this, &Compositor::removeOutput);
    }

    m_state = State::On;

    for (X11Client *c : Workspace::self()->clientList()) {
        c->setupCompositing();
    }
    for (Unmanaged *c : Workspace::self()->unmanagedList()) {
        c->setupCompositing();
    }
    for (InternalClient *client : workspace()->internalClients()) {
        client->setupCompositing();
    }

    if (auto *server = waylandServer()) {
        const auto clients = server->clients();
        for (AbstractClient *c : clients) {
            c->setupCompositing();
        }
    }

    // Sets also the 'effects' pointer.
    kwinApp()->platform()->createEffectsHandler(this, m_scene);

    Q_EMIT compositingToggled(true);

    if (m_releaseSelectionTimer.isActive()) {
        m_releaseSelectionTimer.stop();
    }
}

AbstractOutput *Compositor::findOutput(RenderLoop *loop) const
{
    const auto outputs = kwinApp()->platform()->enabledOutputs();
    for (AbstractOutput *output : outputs) {
        if (output->renderLoop() == loop) {
            return output;
        }
    }
    return nullptr;
}

void Compositor::addOutput(AbstractOutput *output)
{
    Q_ASSERT(kwinApp()->operationMode() != Application::OperationModeX11);

    auto workspaceLayer = new RenderLayer(output->renderLoop());
    workspaceLayer->setDelegate(new SceneDelegate(m_scene, output));
    workspaceLayer->setGeometry(output->geometry());
    connect(output, &AbstractOutput::geometryChanged, workspaceLayer, [output, workspaceLayer]() {
        workspaceLayer->setGeometry(output->geometry());
    });

    auto cursorLayer = new RenderLayer(output->renderLoop());
    cursorLayer->setVisible(false);
    cursorLayer->setDelegate(new CursorDelegate(output, m_cursorView));
    cursorLayer->setParent(workspaceLayer);
    cursorLayer->setSuperlayer(workspaceLayer);

    auto updateCursorLayer = [output, workspaceLayer, cursorLayer]() {
        const Cursor *cursor = Cursors::self()->currentCursor();
        cursorLayer->setVisible(cursor->isOnOutput(output) && output->usesSoftwareCursor());
        cursorLayer->setGeometry(workspaceLayer->mapFromGlobal(cursor->geometry()));
        cursorLayer->addRepaintFull();
    };
    updateCursorLayer();
    connect(output, &AbstractOutput::geometryChanged, cursorLayer, updateCursorLayer);
    connect(Cursors::self(), &Cursors::currentCursorChanged, cursorLayer, updateCursorLayer);
    connect(Cursors::self(), &Cursors::hiddenChanged, cursorLayer, updateCursorLayer);
    connect(Cursors::self(), &Cursors::positionChanged, cursorLayer, updateCursorLayer);

    addSuperLayer(workspaceLayer);
}

void Compositor::removeOutput(AbstractOutput *output)
{
    removeSuperLayer(m_superlayers[output->renderLoop()]);
}

void Compositor::addSuperLayer(RenderLayer *layer)
{
    m_superlayers.insert(layer->loop(), layer);
    connect(layer->loop(), &RenderLoop::frameRequested, this, &Compositor::handleFrameRequested);
}

void Compositor::removeSuperLayer(RenderLayer *layer)
{
    m_superlayers.remove(layer->loop());
    disconnect(layer->loop(), &RenderLoop::frameRequested, this, &Compositor::handleFrameRequested);
    delete layer;
}

void Compositor::scheduleRepaint()
{
    for (auto it = m_superlayers.constBegin(); it != m_superlayers.constEnd(); ++it) {
        it.key()->scheduleRepaint();
    }
}

void Compositor::stop()
{
    if (m_state == State::Off || m_state == State::Stopping) {
        return;
    }
    m_state = State::Stopping;
    Q_EMIT aboutToToggleCompositing();

    m_releaseSelectionTimer.start();

    // Some effects might need access to effect windows when they are about to
    // be destroyed, for example to unreference deleted windows, so we have to
    // make sure that effect windows outlive effects.
    delete effects;
    effects = nullptr;

    if (Workspace::self()) {
        for (X11Client *c : Workspace::self()->clientList()) {
            m_scene->removeToplevel(c);
        }
        for (Unmanaged *c : Workspace::self()->unmanagedList()) {
            m_scene->removeToplevel(c);
        }
        for (InternalClient *client : workspace()->internalClients()) {
            m_scene->removeToplevel(client);
        }
        for (X11Client *c : Workspace::self()->clientList()) {
            c->finishCompositing();
        }
        for (Unmanaged *c : Workspace::self()->unmanagedList()) {
            c->finishCompositing();
        }
        for (InternalClient *client : workspace()->internalClients()) {
            client->finishCompositing();
        }
        if (auto *con = kwinApp()->x11Connection()) {
            xcb_composite_unredirect_subwindows(con, kwinApp()->x11RootWindow(),
                                                XCB_COMPOSITE_REDIRECT_MANUAL);
        }
        while (!workspace()->deletedList().isEmpty()) {
            workspace()->deletedList().first()->discard();
        }
    }

    if (waylandServer()) {
        const QList<AbstractClient *> toRemoveTopLevel = waylandServer()->clients();
        for (AbstractClient *c : toRemoveTopLevel) {
            m_scene->removeToplevel(c);
        }
        const QList<AbstractClient *> toFinishCompositing = waylandServer()->clients();
        for (AbstractClient *c : toFinishCompositing) {
            c->finishCompositing();
        }
    }

    const auto superlayers = m_superlayers;
    for (auto it = superlayers.begin(); it != superlayers.end(); ++it) {
        removeSuperLayer(*it);
    }

    disconnect(kwinApp()->platform(), &Platform::outputEnabled, this, &Compositor::addOutput);
    disconnect(kwinApp()->platform(), &Platform::outputDisabled, this, &Compositor::removeOutput);

    delete m_scene;
    m_scene = nullptr;

    delete m_cursorView;
    m_cursorView = nullptr;

    delete m_backend;
    m_backend = nullptr;

    m_state = State::Off;
    Q_EMIT compositingToggled(false);
}

void Compositor::destroyCompositorSelection()
{
    delete m_selectionOwner;
    m_selectionOwner = nullptr;
}

void Compositor::releaseCompositorSelection()
{
    switch (m_state) {
    case State::On:
        // We are compositing at the moment. Don't release.
        break;
    case State::Off:
        if (m_selectionOwner) {
            qCDebug(KWIN_CORE) << "Releasing compositor selection";
            m_selectionOwner->setOwning(false);
            m_selectionOwner->release();
        }
        break;
    case State::Starting:
    case State::Stopping:
        // Still starting or shutting down the compositor. Starting might fail
        // or after stopping a restart might follow. So test again later on.
        m_releaseSelectionTimer.start();
        break;
    }
}

void Compositor::keepSupportProperty(xcb_atom_t atom)
{
    m_unusedSupportProperties.removeAll(atom);
}

void Compositor::removeSupportProperty(xcb_atom_t atom)
{
    m_unusedSupportProperties << atom;
    m_unusedSupportPropertyTimer.start();
}

void Compositor::deleteUnusedSupportProperties()
{
    if (m_state == State::Starting || m_state == State::Stopping) {
        // Currently still maybe restarting the compositor.
        m_unusedSupportPropertyTimer.start();
        return;
    }
    if (auto *con = kwinApp()->x11Connection()) {
        for (const xcb_atom_t &atom : qAsConst(m_unusedSupportProperties)) {
            // remove property from root window
            xcb_delete_property(con, kwinApp()->x11RootWindow(), atom);
        }
        m_unusedSupportProperties.clear();
    }
}

void Compositor::configChanged()
{
    reinitialize();
}

void Compositor::reinitialize()
{
    // Reparse config. Config options will be reloaded by start()
    kwinApp()->config()->reparseConfiguration();

    // Restart compositing
    stop();
    start();

    if (effects) { // start() may fail
        effects->reconfigure();
    }
}

void Compositor::handleFrameRequested(RenderLoop *renderLoop)
{
    composite(renderLoop);
}

void Compositor::composite(RenderLoop *renderLoop)
{
    if (m_backend->checkGraphicsReset()) {
        qCDebug(KWIN_CORE) << "Graphics reset occurred";
#if KWIN_BUILD_NOTIFICATIONS
        KNotification::event(QStringLiteral("graphicsreset"), i18n("Desktop effects were restarted due to a graphics reset"));
#endif
        reinitialize();
        return;
    }

    AbstractOutput *output = findOutput(renderLoop);
    OutputLayer *outputLayer = output->layer();
    fTraceDuration("Paint (", output->name(), ")");

    RenderLayer *superLayer = m_superlayers[renderLoop];
    prePaintPass(superLayer);
    superLayer->setOutputLayer(outputLayer);

    SurfaceItem *scanoutCandidate = superLayer->delegate()->scanoutCandidate();
    renderLoop->setFullscreenSurface(scanoutCandidate);

    renderLoop->beginFrame();
    bool directScanout = false;
    if (scanoutCandidate) {
        const auto sublayers = superLayer->sublayers();
        const bool scanoutPossible = std::none_of(sublayers.begin(), sublayers.end(), [](RenderLayer *sublayer) {
            return sublayer->isVisible();
        });
        if (scanoutPossible && !output->directScanoutInhibited()) {
            directScanout = m_backend->scanout(output, scanoutCandidate);
        }
    }

    if (directScanout) {
        renderLoop->endFrame();
    } else {
        QRegion surfaceDamage = outputLayer->repaints();
        outputLayer->resetRepaints();
        preparePaintPass(superLayer, &surfaceDamage);

        const QRegion repair = m_backend->beginFrame(output);
        const QRegion bufferDamage = surfaceDamage.united(repair);
        m_backend->aboutToStartPainting(output, bufferDamage);

        paintPass(superLayer, bufferDamage);
        renderLoop->endFrame();
        m_backend->endFrame(output, bufferDamage, surfaceDamage);
    }

    postPaintPass(superLayer);

    // TODO: Put it inside the cursor layer once the cursor layer can be backed by a real output layer.
    if (waylandServer()) {
        const std::chrono::milliseconds frameTime =
            std::chrono::duration_cast<std::chrono::milliseconds>(output->renderLoop()->lastPresentationTimestamp());

        if (!Cursors::self()->isCursorHidden()) {
            Cursor *cursor = Cursors::self()->currentCursor();
            if (cursor->geometry().intersects(output->geometry())) {
                cursor->markAsRendered(frameTime);
            }
        }
    }
}

void Compositor::prePaintPass(RenderLayer *layer)
{
    layer->delegate()->prePaint();
    const auto sublayers = layer->sublayers();
    for (RenderLayer *sublayer : sublayers) {
        prePaintPass(sublayer);
    }
}

void Compositor::postPaintPass(RenderLayer *layer)
{
    layer->delegate()->postPaint();
    const auto sublayers = layer->sublayers();
    for (RenderLayer *sublayer : sublayers) {
        postPaintPass(sublayer);
    }
}

void Compositor::preparePaintPass(RenderLayer *layer, QRegion *repaint)
{
    // TODO: Cull opaque region.
    *repaint += layer->mapToGlobal(layer->repaints() + layer->delegate()->repaints());
    layer->resetRepaints();
    const auto sublayers = layer->sublayers();
    for (RenderLayer *sublayer : sublayers) {
        if (sublayer->isVisible()) {
            preparePaintPass(sublayer, repaint);
        }
    }
}

void Compositor::paintPass(RenderLayer *layer, const QRegion &region)
{
    layer->delegate()->paint(region);

    const auto sublayers = layer->sublayers();
    for (RenderLayer *sublayer : sublayers) {
        if (sublayer->isVisible()) {
            paintPass(sublayer, region);
        }
    }
}

bool Compositor::isActive()
{
    return m_state == State::On;
}

WaylandCompositor::WaylandCompositor(QObject *parent)
    : Compositor(parent)
{
    connect(kwinApp(), &Application::x11ConnectionAboutToBeDestroyed,
            this, &WaylandCompositor::destroyCompositorSelection);
}

WaylandCompositor::~WaylandCompositor()
{
    Q_EMIT aboutToDestroy();
    stop(); // this can't be called in the destructor of Compositor
}

void WaylandCompositor::toggleCompositing()
{
    // For the shortcut. Not possible on Wayland because we always composite.
}

void WaylandCompositor::start()
{
    if (!Compositor::setupStart()) {
        // Internal setup failed, abort.
        return;
    }

    if (Workspace::self()) {
        startupWithWorkspace();
    } else {
        connect(kwinApp(), &Application::workspaceCreated,
                this, &WaylandCompositor::startupWithWorkspace);
    }
}

X11Compositor::X11Compositor(QObject *parent)
    : Compositor(parent)
    , m_suspended(options->isUseCompositing() ? NoReasonSuspend : UserSuspend)
{
    if (qEnvironmentVariableIsSet("KWIN_MAX_FRAMES_TESTED")) {
        m_framesToTestForSafety = qEnvironmentVariableIntValue("KWIN_MAX_FRAMES_TESTED");
    }
}

X11Compositor::~X11Compositor()
{
    Q_EMIT aboutToDestroy();
    stop(); // this can't be called in the destructor of Compositor
}

X11SyncManager *X11Compositor::syncManager() const
{
    return m_syncManager.data();
}

void X11Compositor::toggleCompositing()
{
    if (m_suspended) {
        // Direct user call; clear all bits.
        resume(AllReasonSuspend);
    } else {
        // But only set the user one (sufficient to suspend).
        suspend(UserSuspend);
    }
}

void X11Compositor::reinitialize()
{
    // Resume compositing if suspended.
    m_suspended = NoReasonSuspend;
    Compositor::reinitialize();
}

void X11Compositor::configChanged()
{
    if (m_suspended) {
        stop();
        return;
    }
    Compositor::configChanged();
}

void X11Compositor::suspend(X11Compositor::SuspendReason reason)
{
    Q_ASSERT(reason != NoReasonSuspend);
    m_suspended |= reason;

    if (reason & ScriptSuspend) {
        // When disabled show a shortcut how the user can get back compositing.
        const auto shortcuts = KGlobalAccel::self()->shortcut(
            workspace()->findChild<QAction *>(QStringLiteral("Suspend Compositing")));
        if (!shortcuts.isEmpty()) {
            // Display notification only if there is the shortcut.
            const QString message =
                i18n("Desktop effects have been suspended by another application.<br/>"
                     "You can resume using the '%1' shortcut.",
                     shortcuts.first().toString(QKeySequence::NativeText));
#if KWIN_BUILD_NOTIFICATIONS
            KNotification::event(QStringLiteral("compositingsuspendeddbus"), message);
#endif
        }
    }
    stop();
}

void X11Compositor::resume(X11Compositor::SuspendReason reason)
{
    Q_ASSERT(reason != NoReasonSuspend);
    m_suspended &= ~reason;
    start();
}

void X11Compositor::start()
{
    if (m_suspended) {
        QStringList reasons;
        if (m_suspended & UserSuspend) {
            reasons << QStringLiteral("Disabled by User");
        }
        if (m_suspended & BlockRuleSuspend) {
            reasons << QStringLiteral("Disabled by Window");
        }
        if (m_suspended & ScriptSuspend) {
            reasons << QStringLiteral("Disabled by Script");
        }
        qCInfo(KWIN_CORE) << "Compositing is suspended, reason:" << reasons;
        return;
    } else if (!kwinApp()->platform()->compositingPossible()) {
        qCWarning(KWIN_CORE) << "Compositing is not possible";
        return;
    }
    if (!Compositor::setupStart()) {
        // Internal setup failed, abort.
        return;
    }
    startupWithWorkspace();
    m_syncManager.reset(X11SyncManager::create());
}

void X11Compositor::stop()
{
    m_syncManager.reset();
    Compositor::stop();
}

void X11Compositor::composite(RenderLoop *renderLoop)
{
    if (backend()->overlayWindow() && !isOverlayWindowVisible()) {
        // Return since nothing is visible.
        return;
    }

    QList<Toplevel *> windows = Workspace::self()->xStackingOrder();
    QList<SurfaceItemX11 *> dirtyItems;

    // Reset the damage state of each window and fetch the damage region
    // without waiting for a reply
    for (Toplevel *window : qAsConst(windows)) {
        SurfaceItemX11 *surfaceItem = static_cast<SurfaceItemX11 *>(window->surfaceItem());
        if (surfaceItem->fetchDamage()) {
            dirtyItems.append(surfaceItem);
        }
    }

    if (dirtyItems.count() > 0) {
        if (m_syncManager) {
            m_syncManager->triggerFence();
        }
        xcb_flush(kwinApp()->x11Connection());
    }

    // Get the replies
    for (SurfaceItemX11 *item : qAsConst(dirtyItems)) {
        item->waitForDamage();
    }

    if (m_framesToTestForSafety > 0 && (backend()->compositingType() & OpenGLCompositing)) {
        kwinApp()->platform()->createOpenGLSafePoint(Platform::OpenGLSafePoint::PreFrame);
    }

    Compositor::composite(renderLoop);

    if (m_syncManager) {
        if (!m_syncManager->endFrame()) {
            qCDebug(KWIN_CORE) << "Aborting explicit synchronization with the X command stream.";
            qCDebug(KWIN_CORE) << "Future frames will be rendered unsynchronized.";
            m_syncManager.reset();
        }
    }

    if (m_framesToTestForSafety > 0) {
        if (backend()->compositingType() & OpenGLCompositing) {
            kwinApp()->platform()->createOpenGLSafePoint(Platform::OpenGLSafePoint::PostFrame);
        }
        m_framesToTestForSafety--;
        if (m_framesToTestForSafety == 0 && (backend()->compositingType() & OpenGLCompositing)) {
            kwinApp()->platform()->createOpenGLSafePoint(Platform::OpenGLSafePoint::PostLastGuardedFrame);
        }
    }
}

bool X11Compositor::checkForOverlayWindow(WId w) const
{
    if (!backend()) {
        // No backend, so it cannot be the overlay window.
        return false;
    }
    if (!backend()->overlayWindow()) {
        // No overlay window, it cannot be the overlay.
        return false;
    }
    // Compare the window ID's.
    return w == backend()->overlayWindow()->window();
}

bool X11Compositor::isOverlayWindowVisible() const
{
    if (!backend()) {
        return false;
    }
    if (!backend()->overlayWindow()) {
        return false;
    }
    return backend()->overlayWindow()->isVisible();
}

void X11Compositor::updateClientCompositeBlocking(X11Client *c)
{
    if (c) {
        if (c->isBlockingCompositing()) {
            // Do NOT attempt to call suspend(true) from within the eventchain!
            if (!(m_suspended & BlockRuleSuspend))
                QMetaObject::invokeMethod(
                    this, [this]() {
                        suspend(BlockRuleSuspend);
                    },
                    Qt::QueuedConnection);
        }
    } else if (m_suspended & BlockRuleSuspend) {
        // If !c we just check if we can resume in case a blocking client was lost.
        bool shouldResume = true;

        for (auto it = Workspace::self()->clientList().constBegin();
             it != Workspace::self()->clientList().constEnd(); ++it) {
            if ((*it)->isBlockingCompositing()) {
                shouldResume = false;
                break;
            }
        }
        if (shouldResume) {
            // Do NOT attempt to call suspend(false) from within the eventchain!
            QMetaObject::invokeMethod(
                this, [this]() {
                    resume(BlockRuleSuspend);
                },
                Qt::QueuedConnection);
        }
    }
}

X11Compositor *X11Compositor::self()
{
    return qobject_cast<X11Compositor *>(Compositor::self());
}

}

// included for CompositorSelectionOwner
#include "composite.moc"
