/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 1999, 2000 Matthias Ettrich <ettrich@kde.org>
    SPDX-FileCopyrightText: 2003 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

/*

 This file contains things relevant to direct user actions, such as
 responses to global keyboard shortcuts, or selecting actions
 from the window operations menu.

*/

///////////////////////////////////////////////////////////////////////////////
// NOTE: if you change the menu, keep
//       plasma-desktop/applets/taskmanager/package/contents/ui/ContextMenu.qml
//       in sync
//////////////////////////////////////////////////////////////////////////////

#include "useractions.h"
#include "composite.h"
#include "cursor.h"
#include "effects.h"
#include "input.h"
#include "output.h"
#include "platform.h"
#include "scripting/scripting.h"
#include "virtualdesktops.h"
#include "workspace.h"
#include "x11window.h"

#if KWIN_BUILD_ACTIVITIES
#include "activities.h"
#include <kactivities/info.h>
#endif
#include "appmenu.h"

#include <KProcess>

#include <QAction>
#include <QCheckBox>
#include <QPushButton>
#include <QtConcurrentRun>

#include <KGlobalAccel>
#include <KLazyLocalizedString>
#include <KLocalizedString>
#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QRegularExpression>
#include <kauthorized.h>
#include <kconfig.h>

#include "killwindow.h"
#if KWIN_BUILD_TABBOX
#include "tabbox.h"
#endif

namespace KWin
{

UserActionsMenu::UserActionsMenu(QObject *parent)
    : QObject(parent)
    , m_menu(nullptr)
    , m_desktopMenu(nullptr)
    , m_multipleDesktopsMenu(nullptr)
    , m_screenMenu(nullptr)
    , m_activityMenu(nullptr)
    , m_scriptsMenu(nullptr)
    , m_resizeOperation(nullptr)
    , m_moveOperation(nullptr)
    , m_maximizeOperation(nullptr)
    , m_shadeOperation(nullptr)
    , m_keepAboveOperation(nullptr)
    , m_keepBelowOperation(nullptr)
    , m_fullScreenOperation(nullptr)
    , m_noBorderOperation(nullptr)
    , m_minimizeOperation(nullptr)
    , m_closeOperation(nullptr)
    , m_shortcutOperation(nullptr)
{
}

UserActionsMenu::~UserActionsMenu()
{
    discard();
}

bool UserActionsMenu::isShown() const
{
    return m_menu && m_menu->isVisible();
}

bool UserActionsMenu::hasWindow() const
{
    return m_window && isShown();
}

void UserActionsMenu::close()
{
    if (!m_menu) {
        return;
    }
    m_menu->close();
    m_window.clear();
}

bool UserActionsMenu::isMenuWindow(const Window *window) const
{
    return window && window == m_window;
}

void UserActionsMenu::show(const QRect &pos, Window *window)
{
    Q_ASSERT(window);
    QPointer<Window> windowPtr(window);
    // Presumably window will never be nullptr,
    // but play it safe and make sure not to crash.
    if (windowPtr.isNull()) {
        return;
    }
    if (isShown()) { // recursion
        return;
    }
    if (windowPtr->isDesktop() || windowPtr->isDock()) {
        return;
    }
    if (!KAuthorized::authorizeAction(QStringLiteral("kwin_rmb"))) {
        return;
    }
    m_window = windowPtr;
    init();
    m_window->blockActivityUpdates(true);
    if (kwinApp()->shouldUseWaylandForCompositing()) {
        m_menu->popup(pos.bottomLeft());
    } else {
        m_menu->exec(pos.bottomLeft());
    }
    if (m_window) {
        m_window->blockActivityUpdates(false);
    }
}

void UserActionsMenu::grabInput()
{
    m_menu->windowHandle()->setMouseGrabEnabled(true);
    m_menu->windowHandle()->setKeyboardGrabEnabled(true);
}

void UserActionsMenu::helperDialog(const QString &message, Window *window)
{
    QStringList args;
    QString type;
    auto shortcut = [](const QString &name) {
        QAction *action = Workspace::self()->findChild<QAction *>(name);
        Q_ASSERT(action != nullptr);
        const auto shortcuts = KGlobalAccel::self()->shortcut(action);
        return QStringLiteral("%1 (%2)").arg(action->text(), shortcuts.isEmpty() ? QString() : shortcuts.first().toString(QKeySequence::NativeText));
    };
    if (message == QStringLiteral("noborderaltf3")) {
        args << QStringLiteral("--msgbox") << i18n("You have selected to show a window without its border.\n"
                                                   "Without the border, you will not be able to enable the border "
                                                   "again using the mouse: use the window operations menu instead, "
                                                   "activated using the %1 keyboard shortcut.",
                                                   shortcut(QStringLiteral("Window Operations Menu")));
        type = QStringLiteral("altf3warning");
    } else if (message == QLatin1String("fullscreenaltf3")) {
        args << QStringLiteral("--msgbox") << i18n("You have selected to show a window in fullscreen mode.\n"
                                                   "If the application itself does not have an option to turn the fullscreen "
                                                   "mode off you will not be able to disable it "
                                                   "again using the mouse: use the window operations menu instead, "
                                                   "activated using the %1 keyboard shortcut.",
                                                   shortcut(QStringLiteral("Window Operations Menu")));
        type = QStringLiteral("altf3warning");
    } else {
        Q_UNREACHABLE();
    }
    if (!type.isEmpty()) {
        KConfig cfg(QStringLiteral("kwin_dialogsrc"));
        KConfigGroup cg(&cfg, "Notification Messages"); // Depends on KMessageBox
        if (!cg.readEntry(type, true)) {
            return;
        }
        args << QStringLiteral("--dontagain") << QLatin1String("kwin_dialogsrc:") + type;
    }
    if (window) {
        args << QStringLiteral("--embed") << QString::number(window->window());
    }
    QtConcurrent::run([args]() {
        KProcess::startDetached(QStringLiteral("kdialog"), args);
    });
}

QStringList configModules(bool controlCenter)
{
    QStringList args;
    args << QStringLiteral("kwindecoration");
    if (controlCenter) {
        args << QStringLiteral("kwinoptions");
    } else if (KAuthorized::authorizeControlModule(QStringLiteral("kde-kwinoptions.desktop"))) {
        args << QStringLiteral("kwinactions") << QStringLiteral("kwinfocus") << QStringLiteral("kwinmoving") << QStringLiteral("kwinadvanced")
             << QStringLiteral("kwinrules") << QStringLiteral("kwincompositing") << QStringLiteral("kwineffects")
#if KWIN_BUILD_TABBOX
             << QStringLiteral("kwintabbox")
#endif
             << QStringLiteral("kwinscreenedges")
             << QStringLiteral("kwinscripts");
    }
    return args;
}

void UserActionsMenu::init()
{
    if (m_menu) {
        return;
    }
    m_menu = new QMenu;
    connect(m_menu, &QMenu::aboutToShow, this, &UserActionsMenu::menuAboutToShow);
    connect(m_menu, &QMenu::triggered, this, &UserActionsMenu::slotWindowOperation, Qt::QueuedConnection);

    QMenu *advancedMenu = new QMenu(m_menu);
    connect(advancedMenu, &QMenu::aboutToShow, this, [this, advancedMenu]() {
        if (m_window) {
            advancedMenu->setPalette(m_window->palette());
        }
    });

    auto setShortcut = [](QAction *action, const QString &actionName) {
        const auto shortcuts = KGlobalAccel::self()->shortcut(Workspace::self()->findChild<QAction *>(actionName));
        if (!shortcuts.isEmpty()) {
            action->setShortcut(shortcuts.first());
        }
    };

    m_moveOperation = advancedMenu->addAction(i18n("&Move"));
    m_moveOperation->setIcon(QIcon::fromTheme(QStringLiteral("transform-move")));
    setShortcut(m_moveOperation, QStringLiteral("Window Move"));
    m_moveOperation->setData(Options::UnrestrictedMoveOp);

    m_resizeOperation = advancedMenu->addAction(i18n("&Resize"));
    m_resizeOperation->setIcon(QIcon::fromTheme(QStringLiteral("transform-scale")));
    setShortcut(m_resizeOperation, QStringLiteral("Window Resize"));
    m_resizeOperation->setData(Options::ResizeOp);

    m_keepAboveOperation = advancedMenu->addAction(i18n("Keep &Above Others"));
    m_keepAboveOperation->setIcon(QIcon::fromTheme(QStringLiteral("window-keep-above")));
    setShortcut(m_keepAboveOperation, QStringLiteral("Window Above Other Windows"));
    m_keepAboveOperation->setCheckable(true);
    m_keepAboveOperation->setData(Options::KeepAboveOp);

    m_keepBelowOperation = advancedMenu->addAction(i18n("Keep &Below Others"));
    m_keepBelowOperation->setIcon(QIcon::fromTheme(QStringLiteral("window-keep-below")));
    setShortcut(m_keepBelowOperation, QStringLiteral("Window Below Other Windows"));
    m_keepBelowOperation->setCheckable(true);
    m_keepBelowOperation->setData(Options::KeepBelowOp);

    m_fullScreenOperation = advancedMenu->addAction(i18n("&Fullscreen"));
    m_fullScreenOperation->setIcon(QIcon::fromTheme(QStringLiteral("view-fullscreen")));
    setShortcut(m_fullScreenOperation, QStringLiteral("Window Fullscreen"));
    m_fullScreenOperation->setCheckable(true);
    m_fullScreenOperation->setData(Options::FullScreenOp);

    m_shadeOperation = advancedMenu->addAction(i18n("&Shade"));
    m_shadeOperation->setIcon(QIcon::fromTheme(QStringLiteral("window-shade")));
    setShortcut(m_shadeOperation, QStringLiteral("Window Shade"));
    m_shadeOperation->setCheckable(true);
    m_shadeOperation->setData(Options::ShadeOp);

    m_noBorderOperation = advancedMenu->addAction(i18n("&No Border"));
    m_noBorderOperation->setIcon(QIcon::fromTheme(QStringLiteral("edit-none-border")));
    setShortcut(m_noBorderOperation, QStringLiteral("Window No Border"));
    m_noBorderOperation->setCheckable(true);
    m_noBorderOperation->setData(Options::NoBorderOp);

    advancedMenu->addSeparator();

    m_shortcutOperation = advancedMenu->addAction(i18n("Set Window Short&cut..."));
    m_shortcutOperation->setIcon(QIcon::fromTheme(QStringLiteral("configure-shortcuts")));
    setShortcut(m_shortcutOperation, QStringLiteral("Setup Window Shortcut"));
    m_shortcutOperation->setData(Options::SetupWindowShortcutOp);

    QAction *action = advancedMenu->addAction(i18n("Configure Special &Window Settings..."));
    action->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-windows-actions")));
    action->setData(Options::WindowRulesOp);
    m_rulesOperation = action;

    action = advancedMenu->addAction(i18n("Configure S&pecial Application Settings..."));
    action->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-windows-actions")));
    action->setData(Options::ApplicationRulesOp);
    m_applicationRulesOperation = action;
    if (!kwinApp()->config()->isImmutable() && !KAuthorized::authorizeControlModules(configModules(true)).isEmpty()) {
        advancedMenu->addSeparator();
        action = advancedMenu->addAction(i18nc("Entry in context menu of window decoration to open the configuration module of KWin",
                                               "Configure W&indow Manager..."));
        action->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
        connect(action, &QAction::triggered, this, [this]() {
            // opens the KWin configuration
            QStringList args;
            args << QStringLiteral("--icon") << QStringLiteral("preferences-system-windows");
            const QString path = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                                        QStringLiteral("kservices5/kwinfocus.desktop"));
            if (!path.isEmpty()) {
                args << QStringLiteral("--desktopfile") << path;
            }
            args << configModules(false);
            QProcess *p = new QProcess(this);
            p->setArguments(args);
            p->setProcessEnvironment(kwinApp()->processStartupEnvironment());
            p->setProgram(QStringLiteral("kcmshell5"));
            connect(p, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), p, &QProcess::deleteLater);
            connect(p, &QProcess::errorOccurred, this, [](QProcess::ProcessError e) {
                if (e == QProcess::FailedToStart) {
                    qCDebug(KWIN_CORE) << "Failed to start kcmshell5";
                }
            });
            p->start();
        });
    }

    m_maximizeOperation = m_menu->addAction(i18n("Ma&ximize"));
    m_maximizeOperation->setIcon(QIcon::fromTheme(QStringLiteral("window-maximize")));
    setShortcut(m_maximizeOperation, QStringLiteral("Window Maximize"));
    m_maximizeOperation->setCheckable(true);
    m_maximizeOperation->setData(Options::MaximizeOp);

    m_minimizeOperation = m_menu->addAction(i18n("Mi&nimize"));
    m_minimizeOperation->setIcon(QIcon::fromTheme(QStringLiteral("window-minimize")));
    setShortcut(m_minimizeOperation, QStringLiteral("Window Minimize"));
    m_minimizeOperation->setData(Options::MinimizeOp);

    action = m_menu->addMenu(advancedMenu);
    action->setText(i18n("&More Actions"));
    action->setIcon(QIcon::fromTheme(QStringLiteral("overflow-menu")));

    m_closeOperation = m_menu->addAction(i18n("&Close"));
    m_closeOperation->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    setShortcut(m_closeOperation, QStringLiteral("Window Close"));
    m_closeOperation->setData(Options::CloseOp);
}

void UserActionsMenu::discard()
{
    delete m_menu;
    m_menu = nullptr;
    m_desktopMenu = nullptr;
    m_multipleDesktopsMenu = nullptr;
    m_screenMenu = nullptr;
    m_activityMenu = nullptr;
    m_scriptsMenu = nullptr;
}

void UserActionsMenu::menuAboutToShow()
{
    if (m_window.isNull() || !m_menu) {
        return;
    }

    if (VirtualDesktopManager::self()->count() == 1) {
        delete m_desktopMenu;
        m_desktopMenu = nullptr;
        delete m_multipleDesktopsMenu;
        m_multipleDesktopsMenu = nullptr;
    } else {
        initDesktopPopup();
    }
    if (kwinApp()->platform()->enabledOutputs().count() == 1 || (!m_window->isMovable() && !m_window->isMovableAcrossScreens())) {
        delete m_screenMenu;
        m_screenMenu = nullptr;
    } else {
        initScreenPopup();
    }

    m_menu->setPalette(m_window->palette());
    m_resizeOperation->setEnabled(m_window->isResizable());
    m_moveOperation->setEnabled(m_window->isMovableAcrossScreens());
    m_maximizeOperation->setEnabled(m_window->isMaximizable());
    m_maximizeOperation->setChecked(m_window->maximizeMode() == MaximizeFull);
    m_shadeOperation->setEnabled(m_window->isShadeable());
    m_shadeOperation->setChecked(m_window->shadeMode() != ShadeNone);
    m_keepAboveOperation->setChecked(m_window->keepAbove());
    m_keepBelowOperation->setChecked(m_window->keepBelow());
    m_fullScreenOperation->setEnabled(m_window->userCanSetFullScreen());
    m_fullScreenOperation->setChecked(m_window->isFullScreen());
    m_noBorderOperation->setEnabled(m_window->userCanSetNoBorder());
    m_noBorderOperation->setChecked(m_window->noBorder());
    m_minimizeOperation->setEnabled(m_window->isMinimizable());
    m_closeOperation->setEnabled(m_window->isCloseable());
    m_shortcutOperation->setEnabled(m_window->rules()->checkShortcut(QString()).isNull());

    // drop the existing scripts menu
    delete m_scriptsMenu;
    m_scriptsMenu = nullptr;
    // ask scripts whether they want to add entries for the given window
    QList<QAction *> scriptActions = Scripting::self()->actionsForUserActionMenu(m_window.data(), m_scriptsMenu);
    if (!scriptActions.isEmpty()) {
        m_scriptsMenu = new QMenu(m_menu);
        m_scriptsMenu->setPalette(m_window->palette());
        m_scriptsMenu->addActions(scriptActions);

        QAction *action = m_scriptsMenu->menuAction();
        // set it as the first item after desktop
        m_menu->insertAction(m_closeOperation, action);
        action->setText(i18n("&Extensions"));
    }

    m_rulesOperation->setEnabled(m_window->supportsWindowRules());
    m_applicationRulesOperation->setEnabled(m_window->supportsWindowRules());

    showHideActivityMenu();
}

void UserActionsMenu::showHideActivityMenu()
{
#if KWIN_BUILD_ACTIVITIES
    if (!Activities::self()) {
        return;
    }
    const QStringList &openActivities_ = Activities::self()->running();
    qCDebug(KWIN_CORE) << "activities:" << openActivities_.size();
    if (openActivities_.size() < 2) {
        delete m_activityMenu;
        m_activityMenu = nullptr;
    } else {
        initActivityPopup();
    }
#endif
}

void UserActionsMenu::initDesktopPopup()
{
    if (kwinApp()->operationMode() == Application::OperationModeWaylandOnly || kwinApp()->operationMode() == Application::OperationModeXwayland) {
        if (m_multipleDesktopsMenu) {
            return;
        }

        m_multipleDesktopsMenu = new QMenu(m_menu);
        connect(m_multipleDesktopsMenu, &QMenu::aboutToShow, this, &UserActionsMenu::multipleDesktopsPopupAboutToShow);

        QAction *action = m_multipleDesktopsMenu->menuAction();
        // set it as the first item
        m_menu->insertAction(m_maximizeOperation, action);
        action->setText(i18n("&Desktops"));
        action->setIcon(QIcon::fromTheme(QStringLiteral("virtual-desktops")));

    } else {
        if (m_desktopMenu) {
            return;
        }

        m_desktopMenu = new QMenu(m_menu);
        connect(m_desktopMenu, &QMenu::aboutToShow, this, &UserActionsMenu::desktopPopupAboutToShow);

        QAction *action = m_desktopMenu->menuAction();
        // set it as the first item
        m_menu->insertAction(m_maximizeOperation, action);
        action->setText(i18n("Move to &Desktop"));
        action->setIcon(QIcon::fromTheme(QStringLiteral("virtual-desktops")));
    }
}

void UserActionsMenu::initScreenPopup()
{
    if (m_screenMenu) {
        return;
    }

    m_screenMenu = new QMenu(m_menu);
    connect(m_screenMenu, &QMenu::aboutToShow, this, &UserActionsMenu::screenPopupAboutToShow);

    QAction *action = m_screenMenu->menuAction();
    // set it as the first item after desktop
    m_menu->insertAction(m_activityMenu ? m_activityMenu->menuAction() : m_minimizeOperation, action);
    action->setText(i18n("Move to &Screen"));
    action->setIcon(QIcon::fromTheme(QStringLiteral("computer")));
}

void UserActionsMenu::initActivityPopup()
{
    if (m_activityMenu) {
        return;
    }

    m_activityMenu = new QMenu(m_menu);
    connect(m_activityMenu, &QMenu::triggered, this, &UserActionsMenu::slotToggleOnActivity);
    connect(m_activityMenu, &QMenu::aboutToShow, this, &UserActionsMenu::activityPopupAboutToShow);

    QAction *action = m_activityMenu->menuAction();
    // set it as the first item
    m_menu->insertAction(m_maximizeOperation, action);
    action->setText(i18n("Show in &Activities"));
    action->setIcon(QIcon::fromTheme(QStringLiteral("activities")));
}

void UserActionsMenu::desktopPopupAboutToShow()
{
    if (!m_desktopMenu) {
        return;
    }
    const VirtualDesktopManager *vds = VirtualDesktopManager::self();

    m_desktopMenu->clear();
    if (m_window) {
        m_desktopMenu->setPalette(m_window->palette());
    }
    QActionGroup *group = new QActionGroup(m_desktopMenu);
    QAction *action = m_desktopMenu->addAction(i18n("&All Desktops"));
    connect(action, &QAction::triggered, this, [this]() {
        if (m_window) {
            m_window->setOnAllDesktops(!m_window->isOnAllDesktops());
        }
    });
    action->setCheckable(true);
    if (m_window && m_window->isOnAllDesktops()) {
        action->setChecked(true);
    }
    group->addAction(action);

    if (m_window && m_window->isOnAllDesktops()) {
        action->setChecked(true);
    }
    m_desktopMenu->addSeparator();

    const uint BASE = 10;

    const auto desktops = vds->desktops();
    for (VirtualDesktop *desktop : desktops) {
        const uint legacyId = desktop->x11DesktopNumber();

        QString basic_name(QStringLiteral("%1  %2"));
        if (legacyId < BASE) {
            basic_name.prepend(QLatin1Char('&'));
        }
        action = m_desktopMenu->addAction(basic_name.arg(legacyId).arg(desktop->name().replace(QLatin1Char('&'), QStringLiteral("&&"))));
        connect(action, &QAction::triggered, this, [this, desktop]() {
            if (m_window) {
                workspace()->sendWindowToDesktop(m_window, desktop->x11DesktopNumber(), false);
            }
        });
        action->setCheckable(true);
        group->addAction(action);

        if (m_window && !m_window->isOnAllDesktops() && m_window->isOnDesktop(desktop)) {
            action->setChecked(true);
        }
    }

    m_desktopMenu->addSeparator();
    action = m_desktopMenu->addAction(i18nc("Create a new desktop and move the window there", "&New Desktop"));
    connect(action, &QAction::triggered, this, [this]() {
        if (!m_window) {
            return;
        }
        VirtualDesktopManager *vds = VirtualDesktopManager::self();
        VirtualDesktop *desktop = vds->createVirtualDesktop(vds->count());
        if (desktop) {
            workspace()->sendWindowToDesktop(m_window, desktop->x11DesktopNumber(), false);
        }
    });
    action->setEnabled(vds->count() < vds->maximum());
}

void UserActionsMenu::multipleDesktopsPopupAboutToShow()
{
    if (!m_multipleDesktopsMenu) {
        return;
    }
    VirtualDesktopManager *vds = VirtualDesktopManager::self();

    m_multipleDesktopsMenu->clear();
    if (m_window) {
        m_multipleDesktopsMenu->setPalette(m_window->palette());
    }

    QAction *action = m_multipleDesktopsMenu->addAction(i18n("&All Desktops"));
    connect(action, &QAction::triggered, this, [this]() {
        if (m_window) {
            m_window->setOnAllDesktops(!m_window->isOnAllDesktops());
        }
    });
    action->setCheckable(true);
    if (m_window && m_window->isOnAllDesktops()) {
        action->setChecked(true);
    }

    m_multipleDesktopsMenu->addSeparator();

    const uint BASE = 10;

    const auto desktops = vds->desktops();
    for (VirtualDesktop *desktop : desktops) {
        const uint legacyId = desktop->x11DesktopNumber();

        QString basic_name(QStringLiteral("%1  %2"));
        if (legacyId < BASE) {
            basic_name.prepend(QLatin1Char('&'));
        }

        QAction *action = m_multipleDesktopsMenu->addAction(basic_name.arg(legacyId).arg(desktop->name().replace(QLatin1Char('&'), QStringLiteral("&&"))));
        connect(action, &QAction::triggered, this, [this, desktop]() {
            if (m_window) {
                if (m_window->desktops().contains(desktop)) {
                    m_window->leaveDesktop(desktop);
                } else {
                    m_window->enterDesktop(desktop);
                }
            }
        });
        action->setCheckable(true);
        if (m_window && !m_window->isOnAllDesktops() && m_window->isOnDesktop(desktop)) {
            action->setChecked(true);
        }
    }

    m_multipleDesktopsMenu->addSeparator();

    for (VirtualDesktop *desktop : desktops) {
        const uint legacyId = desktop->x11DesktopNumber();
        QString name = i18n("Move to %1 %2", legacyId, desktop->name());
        QAction *action = m_multipleDesktopsMenu->addAction(name);
        connect(action, &QAction::triggered, this, [this, desktop]() {
            if (m_window) {
                m_window->setDesktops({desktop});
            }
        });
    }

    m_multipleDesktopsMenu->addSeparator();

    bool allowNewDesktops = vds->count() < vds->maximum();

    action = m_multipleDesktopsMenu->addAction(i18nc("Create a new desktop and add the window to that desktop", "Add to &New Desktop"));
    connect(action, &QAction::triggered, this, [this, vds]() {
        if (!m_window) {
            return;
        }
        VirtualDesktop *desktop = vds->createVirtualDesktop(vds->count());
        if (desktop) {
            m_window->enterDesktop(desktop);
        }
    });
    action->setEnabled(allowNewDesktops);

    action = m_multipleDesktopsMenu->addAction(i18nc("Create a new desktop and move the window to that desktop", "Move to New Desktop"));
    connect(action, &QAction::triggered, this, [this, vds]() {
        if (!m_window) {
            return;
        }
        VirtualDesktop *desktop = vds->createVirtualDesktop(vds->count());
        if (desktop) {
            m_window->setDesktops({desktop});
        }
    });
    action->setEnabled(allowNewDesktops);
}

void UserActionsMenu::screenPopupAboutToShow()
{
    if (!m_screenMenu) {
        return;
    }
    m_screenMenu->clear();

    if (!m_window) {
        return;
    }
    m_screenMenu->setPalette(m_window->palette());
    QActionGroup *group = new QActionGroup(m_screenMenu);

    const auto outputs = kwinApp()->platform()->enabledOutputs();
    for (int i = 0; i < outputs.count(); ++i) {
        Output *output = outputs[i];
        // assumption: there are not more than 9 screens attached.
        QAction *action = m_screenMenu->addAction(i18nc("@item:inmenu List of all Screens to send a window to. First argument is a number, second the output identifier. E.g. Screen 1 (HDMI1)",
                                                        "Screen &%1 (%2)", (i + 1), output->name()));
        connect(action, &QAction::triggered, this, [this, output]() {
            workspace()->sendWindowToOutput(m_window, output);
        });
        action->setCheckable(true);
        if (m_window && output == m_window->output()) {
            action->setChecked(true);
        }
        group->addAction(action);
    }
}

void UserActionsMenu::activityPopupAboutToShow()
{
    if (!m_activityMenu) {
        return;
    }

#if KWIN_BUILD_ACTIVITIES
    if (!Activities::self()) {
        return;
    }
    m_activityMenu->clear();
    if (m_window) {
        m_activityMenu->setPalette(m_window->palette());
    }
    QAction *action = m_activityMenu->addAction(i18n("&All Activities"));
    action->setData(QString());
    action->setCheckable(true);
    static QPointer<QActionGroup> allActivitiesGroup;
    if (!allActivitiesGroup) {
        allActivitiesGroup = new QActionGroup(m_activityMenu);
    }
    allActivitiesGroup->addAction(action);

    if (m_window && m_window->isOnAllActivities()) {
        action->setChecked(true);
    }
    m_activityMenu->addSeparator();

    const auto activities = Activities::self()->running();
    for (const QString &id : activities) {
        KActivities::Info activity(id);
        QString name = activity.name();
        name.replace('&', "&&");
        auto action = m_activityMenu->addAction(name);
        action->setCheckable(true);
        const QString icon = activity.icon();
        if (!icon.isEmpty()) {
            action->setIcon(QIcon::fromTheme(icon));
        }
        m_activityMenu->addAction(action);
        action->setData(id);

        if (m_window && !m_window->isOnAllActivities() && m_window->isOnActivity(id)) {
            action->setChecked(true);
        }
    }
#endif
}

void UserActionsMenu::slotWindowOperation(QAction *action)
{
    if (!action->data().isValid()) {
        return;
    }

    Options::WindowOperation op = static_cast<Options::WindowOperation>(action->data().toInt());
    QPointer<Window> c = m_window ? m_window : QPointer<Window>(Workspace::self()->activeWindow());
    if (c.isNull()) {
        return;
    }
    QString type;
    switch (op) {
    case Options::FullScreenOp:
        if (!c->isFullScreen() && c->userCanSetFullScreen()) {
            type = QStringLiteral("fullscreenaltf3");
        }
        break;
    case Options::NoBorderOp:
        if (!c->noBorder() && c->userCanSetNoBorder()) {
            type = QStringLiteral("noborderaltf3");
        }
        break;
    default:
        break;
    }
    if (!type.isEmpty()) {
        helperDialog(type, c);
    }
    // need to delay performing the window operation as we need to have the
    // user actions menu closed before we destroy the decoration. Otherwise Qt crashes
    qRegisterMetaType<Options::WindowOperation>();
    QMetaObject::invokeMethod(workspace(), std::bind(&Workspace::performWindowOperation, workspace(), c, op), Qt::QueuedConnection);
}

void UserActionsMenu::slotToggleOnActivity(QAction *action)
{
#if KWIN_BUILD_ACTIVITIES
    if (!Activities::self()) {
        return;
    }
    QString activity = action->data().toString();
    if (m_window.isNull()) {
        return;
    }
    if (activity.isEmpty()) {
        // the 'on_all_activities' menu entry
        m_window->setOnAllActivities(!m_window->isOnAllActivities());
        return;
    }

    Activities::self()->toggleWindowOnActivity(m_window, activity, false);
    if (m_activityMenu && m_activityMenu->isVisible() && m_activityMenu->actions().count()) {
        const bool isOnAll = m_window->isOnAllActivities();
        m_activityMenu->actions().at(0)->setChecked(isOnAll);
        if (isOnAll) {
            // toggleClientOnActivity interprets "on all" as "on none" and
            // susequent toggling ("off") would move the window to only that activity.
            // bug #330838 -> set all but "on all" off to "force proper usage"
            for (int i = 1; i < m_activityMenu->actions().count(); ++i) {
                m_activityMenu->actions().at(i)->setChecked(true);
            }
        }
    }

#else
    Q_UNUSED(action)
#endif
}

//****************************************
// ShortcutDialog
//****************************************
ShortcutDialog::ShortcutDialog(const QKeySequence &cut)
    : _shortcut(cut)
{
    m_ui.setupUi(this);
    m_ui.keySequenceEdit->setKeySequence(cut);
    m_ui.warning->hide();

    // Listen to changed shortcuts
    connect(m_ui.keySequenceEdit, &QKeySequenceEdit::editingFinished, this, &ShortcutDialog::keySequenceChanged);
    connect(m_ui.clearButton, &QToolButton::clicked, this, [this] {
        _shortcut = QKeySequence();
    });
    m_ui.keySequenceEdit->setFocus();

    setWindowFlags(Qt::Popup | Qt::X11BypassWindowManagerHint);
}

void ShortcutDialog::accept()
{
    QKeySequence seq = shortcut();
    if (!seq.isEmpty()) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        if (seq[0] == Qt::Key_Escape) {
#else
        if (seq[0] == QKeyCombination(Qt::Key_Escape)) {
#endif
            reject();
            return;
        }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        if (seq[0] == Qt::Key_Space
            || (seq[0] & Qt::KeyboardModifierMask) == 0) {
#else
        if (seq[0] == QKeyCombination(Qt::Key_Space) || seq[0].keyboardModifiers() == Qt::NoModifier) {
#endif
            // clear
            m_ui.keySequenceEdit->clear();
            QDialog::accept();
            return;
        }
    }
    QDialog::accept();
}

void ShortcutDialog::done(int r)
{
    QDialog::done(r);
    Q_EMIT dialogDone(r == Accepted);
}

void ShortcutDialog::keySequenceChanged()
{
    activateWindow(); // where is the kbd focus lost? cause of popup state?
    QKeySequence seq = m_ui.keySequenceEdit->keySequence();
    if (_shortcut == seq) {
        return; // don't try to update the same
    }

    if (seq.isEmpty()) { // clear
        _shortcut = seq;
        return;
    }
    if (seq.count() > 1) {
        seq = QKeySequence(seq[0]);
        m_ui.keySequenceEdit->setKeySequence(seq);
    }

    // Check if the key sequence is used currently
    QString sc = seq.toString();
    // NOTICE - seq.toString() & the entries in "conflicting" randomly get invalidated after the next call (if no sc has been set & conflicting isn't empty?!)
    QList<KGlobalShortcutInfo> conflicting = KGlobalAccel::globalShortcutsByKey(seq);
    if (!conflicting.isEmpty()) {
        const KGlobalShortcutInfo &conflict = conflicting.at(0);
        m_ui.warning->setText(i18nc("'%1' is a keyboard shortcut like 'ctrl+w'",
                                    "<b>%1</b> is already in use", sc));
        m_ui.warning->setToolTip(i18nc("keyboard shortcut '%1' is used by action '%2' in application '%3'",
                                       "<b>%1</b> is used by %2 in %3", sc, conflict.friendlyName(), conflict.componentFriendlyName()));
        m_ui.warning->show();
        m_ui.keySequenceEdit->setKeySequence(shortcut());
    } else if (seq != _shortcut) {
        m_ui.warning->hide();
        if (QPushButton *ok = m_ui.buttonBox->button(QDialogButtonBox::Ok)) {
            ok->setFocus();
        }
    }

    _shortcut = seq;
}

QKeySequence ShortcutDialog::shortcut() const
{
    return _shortcut;
}

//****************************************
// Workspace
//****************************************

void Workspace::slotIncreaseWindowOpacity()
{
    if (!m_activeWindow) {
        return;
    }
    m_activeWindow->setOpacity(qMin(m_activeWindow->opacity() + 0.05, 1.0));
}

void Workspace::slotLowerWindowOpacity()
{
    if (!m_activeWindow) {
        return;
    }
    m_activeWindow->setOpacity(qMax(m_activeWindow->opacity() - 0.05, 0.05));
}

void Workspace::closeActivePopup()
{
    if (active_popup) {
        active_popup->close();
        active_popup = nullptr;
        m_activePopupWindow = nullptr;
    }
    m_userActionsMenu->close();
}

template<typename Slot>
void Workspace::initShortcut(const QString &actionName, const QString &description, const QKeySequence &shortcut, Slot slot, const QVariant &data)
{
    initShortcut(actionName, description, shortcut, this, slot, data);
}

template<typename T, typename Slot>
void Workspace::initShortcut(const QString &actionName, const QString &description, const QKeySequence &shortcut, T *receiver, Slot slot, const QVariant &data)
{
    QAction *a = new QAction(this);
    a->setProperty("componentName", QStringLiteral(KWIN_NAME));
    a->setObjectName(actionName);
    a->setText(description);
    if (data.isValid()) {
        a->setData(data);
    }
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << shortcut);
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << shortcut);
    input()->registerShortcut(shortcut, a, receiver, slot);
}

/**
 * Creates the global accel object \c keys.
 */
void Workspace::initShortcuts()
{
    // Some shortcuts have Tarzan-speech like names, they need extra
    // normal human descriptions with DEF2() the others can use DEF()
    // new DEF3 allows to pass data to the action, replacing the %1 argument in the name

#define DEF2(name, descr, key, fnSlot) \
    initShortcut(QStringLiteral(name), descr.toString(), key, &Workspace::fnSlot);

#define DEF(name, key, fnSlot) \
    initShortcut(QString::fromUtf8(name.untranslatedText()), name.toString(), key, &Workspace::fnSlot);

#define DEF3(name, key, fnSlot, value) \
    initShortcut(QString::fromUtf8(name.untranslatedText()).arg(value), name.subs(value).toString(), key, &Workspace::fnSlot, value);

#define DEF4(name, descr, key, functor) \
    initShortcut(QStringLiteral(name), descr.toString(), key, functor);

#define DEF5(name, key, functor, value) \
    initShortcut(QString::fromUtf8(name.untranslatedText()).arg(value), name.subs(value).toString(), key, functor, value);

#define DEF6(name, key, target, fnSlot) \
    initShortcut(QString::fromUtf8(name.untranslatedText()), name.toString(), key, target, &fnSlot);

    DEF(kli18n("Window Operations Menu"),
        Qt::ALT | Qt::Key_F3, slotWindowOperations);
    DEF2("Window Close", kli18n("Close Window"),
         Qt::ALT | Qt::Key_F4, slotWindowClose);
    DEF2("Window Maximize", kli18n("Maximize Window"),
         Qt::META | Qt::Key_PageUp, slotWindowMaximize);
    DEF2("Window Maximize Vertical", kli18n("Maximize Window Vertically"),
         0, slotWindowMaximizeVertical);
    DEF2("Window Maximize Horizontal", kli18n("Maximize Window Horizontally"),
         0, slotWindowMaximizeHorizontal);
    DEF2("Window Minimize", kli18n("Minimize Window"),
         Qt::META | Qt::Key_PageDown, slotWindowMinimize);
    DEF2("Window Shade", kli18n("Shade Window"),
         0, slotWindowShade);
    DEF2("Window Move", kli18n("Move Window"),
         0, slotWindowMove);
    DEF2("Window Resize", kli18n("Resize Window"),
         0, slotWindowResize);
    DEF2("Window Raise", kli18n("Raise Window"),
         0, slotWindowRaise);
    DEF2("Window Lower", kli18n("Lower Window"),
         0, slotWindowLower);
    DEF(kli18n("Toggle Window Raise/Lower"),
        0, slotWindowRaiseOrLower);
    DEF2("Window Fullscreen", kli18n("Make Window Fullscreen"),
         0, slotWindowFullScreen);
    DEF2("Window No Border", kli18n("Hide Window Border"),
         0, slotWindowNoBorder);
    DEF2("Window Above Other Windows", kli18n("Keep Window Above Others"),
         0, slotWindowAbove);
    DEF2("Window Below Other Windows", kli18n("Keep Window Below Others"),
         0, slotWindowBelow);
    DEF(kli18n("Activate Window Demanding Attention"),
        Qt::META | Qt::CTRL | Qt::Key_A, slotActivateAttentionWindow);
    DEF(kli18n("Setup Window Shortcut"),
        0, slotSetupWindowShortcut);
    DEF2("Window Move Center", kli18n("Move Window to the Center"), 0,
         slotWindowCenter);
    DEF2("Window Pack Right", kli18n("Move Window Right"),
         0, slotWindowMoveRight);
    DEF2("Window Pack Left", kli18n("Move Window Left"),
         0, slotWindowMoveLeft);
    DEF2("Window Pack Up", kli18n("Move Window Up"),
         0, slotWindowMoveUp);
    DEF2("Window Pack Down", kli18n("Move Window Down"),
         0, slotWindowMoveDown);
    DEF2("Window Grow Horizontal", kli18n("Expand Window Horizontally"),
         0, slotWindowExpandHorizontal);
    DEF2("Window Grow Vertical", kli18n("Expand Window Vertically"),
         0, slotWindowExpandVertical);
    DEF2("Window Shrink Horizontal", kli18n("Shrink Window Horizontally"),
         0, slotWindowShrinkHorizontal);
    DEF2("Window Shrink Vertical", kli18n("Shrink Window Vertically"),
         0, slotWindowShrinkVertical);
    DEF4("Window Quick Tile Left", kli18n("Quick Tile Window to the Left"),
         Qt::META | Qt::Key_Left, std::bind(&Workspace::quickTileWindow, this, QuickTileFlag::Left));
    DEF4("Window Quick Tile Right", kli18n("Quick Tile Window to the Right"),
         Qt::META | Qt::Key_Right, std::bind(&Workspace::quickTileWindow, this, QuickTileFlag::Right));
    DEF4("Window Quick Tile Top", kli18n("Quick Tile Window to the Top"),
         Qt::META | Qt::Key_Up, std::bind(&Workspace::quickTileWindow, this, QuickTileFlag::Top));
    DEF4("Window Quick Tile Bottom", kli18n("Quick Tile Window to the Bottom"),
         Qt::META | Qt::Key_Down, std::bind(&Workspace::quickTileWindow, this, QuickTileFlag::Bottom));
    DEF4("Window Quick Tile Top Left", kli18n("Quick Tile Window to the Top Left"),
         0, std::bind(&Workspace::quickTileWindow, this, QuickTileFlag::Top | QuickTileFlag::Left));
    DEF4("Window Quick Tile Bottom Left", kli18n("Quick Tile Window to the Bottom Left"),
         0, std::bind(&Workspace::quickTileWindow, this, QuickTileFlag::Bottom | QuickTileFlag::Left));
    DEF4("Window Quick Tile Top Right", kli18n("Quick Tile Window to the Top Right"),
         0, std::bind(&Workspace::quickTileWindow, this, QuickTileFlag::Top | QuickTileFlag::Right));
    DEF4("Window Quick Tile Bottom Right", kli18n("Quick Tile Window to the Bottom Right"),
         0, std::bind(&Workspace::quickTileWindow, this, QuickTileFlag::Bottom | QuickTileFlag::Right));
    DEF4("Switch Window Up", kli18n("Switch to Window Above"),
         Qt::META | Qt::ALT | Qt::Key_Up, std::bind(static_cast<void (Workspace::*)(Direction)>(&Workspace::switchWindow), this, DirectionNorth));
    DEF4("Switch Window Down", kli18n("Switch to Window Below"),
         Qt::META | Qt::ALT | Qt::Key_Down, std::bind(static_cast<void (Workspace::*)(Direction)>(&Workspace::switchWindow), this, DirectionSouth));
    DEF4("Switch Window Right", kli18n("Switch to Window to the Right"),
         Qt::META | Qt::ALT | Qt::Key_Right, std::bind(static_cast<void (Workspace::*)(Direction)>(&Workspace::switchWindow), this, DirectionEast));
    DEF4("Switch Window Left", kli18n("Switch to Window to the Left"),
         Qt::META | Qt::ALT | Qt::Key_Left, std::bind(static_cast<void (Workspace::*)(Direction)>(&Workspace::switchWindow), this, DirectionWest));
    DEF2("Increase Opacity", kli18n("Increase Opacity of Active Window by 5 %"),
         0, slotIncreaseWindowOpacity);
    DEF2("Decrease Opacity", kli18n("Decrease Opacity of Active Window by 5 %"),
         0, slotLowerWindowOpacity);

    DEF2("Window On All Desktops", kli18n("Keep Window on All Desktops"),
         0, slotWindowOnAllDesktops);

    VirtualDesktopManager *vds = VirtualDesktopManager::self();
    for (uint i = 0; i < vds->maximum(); ++i) {
        auto handler = [this, i]() {
            const QVector<VirtualDesktop *> desktops = VirtualDesktopManager::self()->desktops();
            if (i < uint(desktops.count())) {
                slotWindowToDesktop(desktops[i]);
            }
        };
        DEF5(kli18n("Window to Desktop %1"), 0, handler, i + 1);
    }
    DEF(kli18n("Window to Next Desktop"), 0, slotWindowToNextDesktop);
    DEF(kli18n("Window to Previous Desktop"), 0, slotWindowToPreviousDesktop);
    DEF(kli18n("Window One Desktop to the Right"), Qt::META | Qt::CTRL | Qt::SHIFT | Qt::Key_Right, slotWindowToDesktopRight);
    DEF(kli18n("Window One Desktop to the Left"), Qt::META | Qt::CTRL | Qt::SHIFT | Qt::Key_Left, slotWindowToDesktopLeft);
    DEF(kli18n("Window One Desktop Up"), Qt::META | Qt::CTRL | Qt::SHIFT | Qt::Key_Up, slotWindowToDesktopUp);
    DEF(kli18n("Window One Desktop Down"), Qt::META | Qt::CTRL | Qt::SHIFT | Qt::Key_Down, slotWindowToDesktopDown);

    for (int i = 0; i < 8; ++i) {
        DEF3(kli18n("Window to Screen %1"), 0, slotWindowToScreen, i);
    }
    DEF(kli18n("Window to Next Screen"), Qt::META | Qt::SHIFT | Qt::Key_Right, slotWindowToNextScreen);
    DEF(kli18n("Window to Previous Screen"), Qt::META | Qt::SHIFT | Qt::Key_Left, slotWindowToPrevScreen);
    DEF2("Show Desktop", kli18n("Peek at Desktop"), Qt::META | Qt::Key_D, slotToggleShowDesktop);

    for (int i = 0; i < 8; ++i) {
        DEF3(kli18n("Switch to Screen %1"), 0, slotSwitchToScreen, i);
    }

    DEF(kli18n("Switch to Next Screen"), 0, slotSwitchToNextScreen);
    DEF(kli18n("Switch to Previous Screen"), 0, slotSwitchToPrevScreen);

    DEF(kli18n("Kill Window"), Qt::META | Qt::CTRL | Qt::Key_Escape, slotKillWindow);
    DEF6(kli18n("Suspend Compositing"), Qt::SHIFT | Qt::ALT | Qt::Key_F12, Compositor::self(), Compositor::toggleCompositing);
    DEF6(kli18n("Invert Screen Colors"), 0, kwinApp()->platform(), Platform::invertScreen);

#undef DEF
#undef DEF2
#undef DEF3
#undef DEF4
#undef DEF5
#undef DEF6
#if KWIN_BUILD_TABBOX
    TabBox::TabBox::self()->initShortcuts();
#endif
    vds->initShortcuts();
    m_userActionsMenu->discard(); // so that it's recreated next time
}

void Workspace::setupWindowShortcut(Window *window)
{
    Q_ASSERT(m_windowKeysDialog == nullptr);
    // TODO: PORT ME (KGlobalAccel related)
    // keys->setEnabled( false );
    // disable_shortcuts_keys->setEnabled( false );
    // client_keys->setEnabled( false );
    m_windowKeysDialog = new ShortcutDialog(window->shortcut());
    m_windowKeysWindow = window;
    connect(m_windowKeysDialog, &ShortcutDialog::dialogDone, this, &Workspace::setupWindowShortcutDone);
    QRect r = clientArea(ScreenArea, window).toRect();
    QSize size = m_windowKeysDialog->sizeHint();
    QPointF pos = window->pos() + window->clientPos();
    if (pos.x() + size.width() >= r.right()) {
        pos.setX(r.right() - size.width());
    }
    if (pos.y() + size.height() >= r.bottom()) {
        pos.setY(r.bottom() - size.height());
    }
    m_windowKeysDialog->move(pos.toPoint());
    m_windowKeysDialog->show();
    active_popup = m_windowKeysDialog;
    m_activePopupWindow = window;
}

void Workspace::setupWindowShortcutDone(bool ok)
{
    //    keys->setEnabled( true );
    //    disable_shortcuts_keys->setEnabled( true );
    //    client_keys->setEnabled( true );
    if (ok) {
        m_windowKeysWindow->setShortcut(m_windowKeysDialog->shortcut().toString());
    }
    closeActivePopup();
    m_windowKeysDialog->deleteLater();
    m_windowKeysDialog = nullptr;
    m_windowKeysWindow = nullptr;
    if (m_activeWindow) {
        m_activeWindow->takeFocus();
    }
}

void Workspace::windowShortcutUpdated(Window *window)
{
    QString key = QStringLiteral("_k_session:%1").arg(window->window());
    QAction *action = findChild<QAction *>(key);
    if (!window->shortcut().isEmpty()) {
        if (action == nullptr) { // new shortcut
            action = new QAction(this);
            kwinApp()->platform()->setupActionForGlobalAccel(action);
            action->setProperty("componentName", QStringLiteral(KWIN_NAME));
            action->setObjectName(key);
            action->setText(i18n("Activate Window (%1)", window->caption()));
            connect(action, &QAction::triggered, window, std::bind(&Workspace::activateWindow, this, window, true));
        }

        // no autoloading, since it's configured explicitly here and is not meant to be reused
        // (the key is the window id anyway, which is kind of random)
        KGlobalAccel::self()->setShortcut(action, QList<QKeySequence>() << window->shortcut(),
                                          KGlobalAccel::NoAutoloading);
        action->setEnabled(true);
    } else {
        KGlobalAccel::self()->removeAllShortcuts(action);
        delete action;
    }
}

void Workspace::performWindowOperation(Window *window, Options::WindowOperation op)
{
    if (!window) {
        return;
    }
    if (op == Options::MoveOp || op == Options::UnrestrictedMoveOp) {
        Cursors::self()->mouse()->setPos(window->frameGeometry().center());
    }
    if (op == Options::ResizeOp || op == Options::UnrestrictedResizeOp) {
        Cursors::self()->mouse()->setPos(window->frameGeometry().bottomRight());
    }
    switch (op) {
    case Options::MoveOp:
        window->performMouseCommand(Options::MouseMove, Cursors::self()->mouse()->pos());
        break;
    case Options::UnrestrictedMoveOp:
        window->performMouseCommand(Options::MouseUnrestrictedMove, Cursors::self()->mouse()->pos());
        break;
    case Options::ResizeOp:
        window->performMouseCommand(Options::MouseResize, Cursors::self()->mouse()->pos());
        break;
    case Options::UnrestrictedResizeOp:
        window->performMouseCommand(Options::MouseUnrestrictedResize, Cursors::self()->mouse()->pos());
        break;
    case Options::CloseOp:
        QMetaObject::invokeMethod(window, &Window::closeWindow, Qt::QueuedConnection);
        break;
    case Options::MaximizeOp:
        window->maximize(window->maximizeMode() == MaximizeFull
                             ? MaximizeRestore
                             : MaximizeFull);
        takeActivity(window, ActivityFocus | ActivityRaise);
        break;
    case Options::HMaximizeOp:
        window->maximize(window->maximizeMode() ^ MaximizeHorizontal);
        takeActivity(window, ActivityFocus | ActivityRaise);
        break;
    case Options::VMaximizeOp:
        window->maximize(window->maximizeMode() ^ MaximizeVertical);
        takeActivity(window, ActivityFocus | ActivityRaise);
        break;
    case Options::RestoreOp:
        window->maximize(MaximizeRestore);
        takeActivity(window, ActivityFocus | ActivityRaise);
        break;
    case Options::MinimizeOp:
        window->minimize();
        break;
    case Options::ShadeOp:
        window->performMouseCommand(Options::MouseShade, Cursors::self()->mouse()->pos());
        break;
    case Options::OnAllDesktopsOp:
        window->setOnAllDesktops(!window->isOnAllDesktops());
        break;
    case Options::FullScreenOp:
        window->setFullScreen(!window->isFullScreen(), true);
        break;
    case Options::NoBorderOp:
        if (window->userCanSetNoBorder()) {
            window->setNoBorder(!window->noBorder());
        }
        break;
    case Options::KeepAboveOp: {
        StackingUpdatesBlocker blocker(this);
        bool was = window->keepAbove();
        window->setKeepAbove(!window->keepAbove());
        if (was && !window->keepAbove()) {
            raiseWindow(window);
        }
        break;
    }
    case Options::KeepBelowOp: {
        StackingUpdatesBlocker blocker(this);
        bool was = window->keepBelow();
        window->setKeepBelow(!window->keepBelow());
        if (was && !window->keepBelow()) {
            lowerWindow(window);
        }
        break;
    }
    case Options::OperationsOp:
        window->performMouseCommand(Options::MouseShade, Cursors::self()->mouse()->pos());
        break;
    case Options::WindowRulesOp:
        RuleBook::self()->edit(window, false);
        break;
    case Options::ApplicationRulesOp:
        RuleBook::self()->edit(window, true);
        break;
    case Options::SetupWindowShortcutOp:
        setupWindowShortcut(window);
        break;
    case Options::LowerOp:
        lowerWindow(window);
        break;
    case Options::NoOp:
        break;
    }
}

void Workspace::slotActivateAttentionWindow()
{
    if (attention_chain.count() > 0) {
        activateWindow(attention_chain.first());
    }
}

static uint senderValue(QObject *sender)
{
    QAction *act = qobject_cast<QAction *>(sender);
    bool ok = false;
    uint i = -1;
    if (act) {
        i = act->data().toUInt(&ok);
    }
    if (ok) {
        return i;
    }
    return -1;
}

#define USABLE_ACTIVE_WINDOW (m_activeWindow && !(m_activeWindow->isDesktop() || m_activeWindow->isDock()))

void Workspace::slotWindowToDesktop(VirtualDesktop *desktop)
{
    if (USABLE_ACTIVE_WINDOW) {
        sendWindowToDesktop(m_activeWindow, desktop->x11DesktopNumber(), true);
    }
}

static bool screenSwitchImpossible()
{
    if (!options->activeMouseScreen()) {
        return false;
    }
    QStringList args;
    args << QStringLiteral("--passivepopup") << i18n("The window manager is configured to consider the screen with the mouse on it as active one.\n"
                                                     "Therefore it is not possible to switch to a screen explicitly.")
         << QStringLiteral("20");
    KProcess::startDetached(QStringLiteral("kdialog"), args);
    return true;
}

Output *Workspace::nextOutput(Output *reference) const
{
    const auto outputs = kwinApp()->platform()->enabledOutputs();
    const int index = outputs.indexOf(reference);
    Q_ASSERT(index != -1);
    return outputs[(index + 1) % outputs.count()];
}

Output *Workspace::previousOutput(Output *reference) const
{
    const auto outputs = kwinApp()->platform()->enabledOutputs();
    const int index = outputs.indexOf(reference);
    Q_ASSERT(index != -1);
    return outputs[(index + outputs.count() - 1) % outputs.count()];
}

void Workspace::slotSwitchToScreen()
{
    if (screenSwitchImpossible()) {
        return;
    }
    Output *output = kwinApp()->platform()->findOutput(senderValue(sender()));
    if (output) {
        switchToOutput(output);
    }
}

void Workspace::slotSwitchToNextScreen()
{
    if (screenSwitchImpossible()) {
        return;
    }
    switchToOutput(nextOutput(activeOutput()));
}

void Workspace::slotSwitchToPrevScreen()
{
    if (screenSwitchImpossible()) {
        return;
    }
    switchToOutput(previousOutput(activeOutput()));
}

void Workspace::slotWindowToScreen()
{
    if (USABLE_ACTIVE_WINDOW) {
        Output *output = kwinApp()->platform()->findOutput(senderValue(sender()));
        if (output) {
            sendWindowToOutput(m_activeWindow, output);
        }
    }
}

void Workspace::slotWindowToNextScreen()
{
    if (USABLE_ACTIVE_WINDOW) {
        sendWindowToOutput(m_activeWindow, nextOutput(m_activeWindow->output()));
    }
}

void Workspace::slotWindowToPrevScreen()
{
    if (USABLE_ACTIVE_WINDOW) {
        sendWindowToOutput(m_activeWindow, previousOutput(m_activeWindow->output()));
    }
}

/**
 * Maximizes the active window.
 */
void Workspace::slotWindowMaximize()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::MaximizeOp);
    }
}

/**
 * Maximizes the active window vertically.
 */
void Workspace::slotWindowMaximizeVertical()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::VMaximizeOp);
    }
}

/**
 * Maximizes the active window horiozontally.
 */
void Workspace::slotWindowMaximizeHorizontal()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::HMaximizeOp);
    }
}

/**
 * Minimizes the active window.
 */
void Workspace::slotWindowMinimize()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::MinimizeOp);
    }
}

/**
 * Shades/unshades the active window respectively.
 */
void Workspace::slotWindowShade()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::ShadeOp);
    }
}

/**
 * Raises the active window.
 */
void Workspace::slotWindowRaise()
{
    if (USABLE_ACTIVE_WINDOW) {
        raiseWindow(m_activeWindow);
    }
}

/**
 * Lowers the active window.
 */
void Workspace::slotWindowLower()
{
    if (USABLE_ACTIVE_WINDOW) {
        lowerWindow(m_activeWindow);
        // As this most likely makes the window no longer visible change the
        // keyboard focus to the next available window.
        // activateNextWindow( c ); // Doesn't work when we lower a child window
        if (m_activeWindow->isActive() && options->focusPolicyIsReasonable()) {
            if (options->isNextFocusPrefersMouse()) {
                Window *next = windowUnderMouse(m_activeWindow->output());
                if (next && next != m_activeWindow) {
                    requestFocus(next, false);
                }
            } else {
                activateWindow(topWindowOnDesktop(VirtualDesktopManager::self()->currentDesktop()));
            }
        }
    }
}

/**
 * Does a toggle-raise-and-lower on the active window.
 */
void Workspace::slotWindowRaiseOrLower()
{
    if (USABLE_ACTIVE_WINDOW) {
        raiseOrLowerWindow(m_activeWindow);
    }
}

void Workspace::slotWindowOnAllDesktops()
{
    if (USABLE_ACTIVE_WINDOW) {
        m_activeWindow->setOnAllDesktops(!m_activeWindow->isOnAllDesktops());
    }
}

void Workspace::slotWindowFullScreen()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::FullScreenOp);
    }
}

void Workspace::slotWindowNoBorder()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::NoBorderOp);
    }
}

void Workspace::slotWindowAbove()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::KeepAboveOp);
    }
}

void Workspace::slotWindowBelow()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::KeepBelowOp);
    }
}
void Workspace::slotSetupWindowShortcut()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::SetupWindowShortcutOp);
    }
}

/**
 * Toggles show desktop.
 */
void Workspace::slotToggleShowDesktop()
{
    setShowingDesktop(!showingDesktop());
}

template<typename Direction>
void windowToDesktop(Window *window)
{
    VirtualDesktopManager *vds = VirtualDesktopManager::self();
    Workspace *ws = Workspace::self();
    Direction functor;
    // TODO: why is options->isRollOverDesktops() not honored?
    const auto desktop = functor(nullptr, true);
    if (window && !window->isDesktop()
        && !window->isDock()) {
        ws->setMoveResizeWindow(window);
        vds->setCurrent(desktop);
        ws->setMoveResizeWindow(nullptr);
    }
}

/**
 * Moves the active window to the next desktop.
 */
void Workspace::slotWindowToNextDesktop()
{
    if (USABLE_ACTIVE_WINDOW) {
        windowToNextDesktop(m_activeWindow);
    }
}

void Workspace::windowToNextDesktop(Window *window)
{
    windowToDesktop<DesktopNext>(window);
}

/**
 * Moves the active window to the previous desktop.
 */
void Workspace::slotWindowToPreviousDesktop()
{
    if (USABLE_ACTIVE_WINDOW) {
        windowToPreviousDesktop(m_activeWindow);
    }
}

void Workspace::windowToPreviousDesktop(Window *window)
{
    windowToDesktop<DesktopPrevious>(window);
}

template<typename Direction>
void activeWindowToDesktop()
{
    VirtualDesktopManager *vds = VirtualDesktopManager::self();
    Workspace *ws = Workspace::self();
    VirtualDesktop *current = vds->currentDesktop();
    Direction functor;
    VirtualDesktop *newCurrent = functor(current, options->isRollOverDesktops());
    if (newCurrent == current) {
        return;
    }
    ws->setMoveResizeWindow(ws->activeWindow());
    vds->setCurrent(newCurrent);
    ws->setMoveResizeWindow(nullptr);
}

void Workspace::slotWindowToDesktopRight()
{
    if (USABLE_ACTIVE_WINDOW) {
        activeWindowToDesktop<DesktopRight>();
    }
}

void Workspace::slotWindowToDesktopLeft()
{
    if (USABLE_ACTIVE_WINDOW) {
        activeWindowToDesktop<DesktopLeft>();
    }
}

void Workspace::slotWindowToDesktopUp()
{
    if (USABLE_ACTIVE_WINDOW) {
        activeWindowToDesktop<DesktopAbove>();
    }
}

void Workspace::slotWindowToDesktopDown()
{
    if (USABLE_ACTIVE_WINDOW) {
        activeWindowToDesktop<DesktopBelow>();
    }
}

/**
 * Kill Window feature, similar to xkill.
 */
void Workspace::slotKillWindow()
{
    if (m_windowKiller.isNull()) {
        m_windowKiller.reset(new KillWindow());
    }
    m_windowKiller->start();
}

/**
 * Switches to the nearest window in given direction.
 */
void Workspace::switchWindow(Direction direction)
{
    if (!m_activeWindow) {
        return;
    }
    Window *window = m_activeWindow;
    VirtualDesktop *desktop = VirtualDesktopManager::self()->currentDesktop();

    // Centre of the active window
    QPoint curPos(window->x() + window->width() / 2, window->y() + window->height() / 2);

    if (!switchWindow(window, direction, curPos, desktop)) {
        auto opposite = [&] {
            switch (direction) {
            case DirectionNorth:
                return QPoint(curPos.x(), geometry().height());
            case DirectionSouth:
                return QPoint(curPos.x(), 0);
            case DirectionEast:
                return QPoint(0, curPos.y());
            case DirectionWest:
                return QPoint(geometry().width(), curPos.y());
            default:
                Q_UNREACHABLE();
            }
        };

        switchWindow(window, direction, opposite(), desktop);
    }
}

bool Workspace::switchWindow(Window *window, Direction direction, QPoint curPos, VirtualDesktop *desktop)
{
    Window *switchTo = nullptr;
    int bestScore = 0;

    QList<Window *> clist = stackingOrder();
    for (auto i = clist.rbegin(); i != clist.rend(); ++i) {
        auto other = *i;
        if (!other->isClient()) {
            continue;
        }
        if (other->wantsTabFocus() && *i != window && other->isOnDesktop(desktop) && !other->isMinimized() && (*i)->isOnCurrentActivity()) {
            // Centre of the other window
            const QPoint otherCenter(other->x() + other->width() / 2, other->y() + other->height() / 2);

            int distance;
            int offset;
            switch (direction) {
            case DirectionNorth:
                distance = curPos.y() - otherCenter.y();
                offset = qAbs(otherCenter.x() - curPos.x());
                break;
            case DirectionEast:
                distance = otherCenter.x() - curPos.x();
                offset = qAbs(otherCenter.y() - curPos.y());
                break;
            case DirectionSouth:
                distance = otherCenter.y() - curPos.y();
                offset = qAbs(otherCenter.x() - curPos.x());
                break;
            case DirectionWest:
                distance = curPos.x() - otherCenter.x();
                offset = qAbs(otherCenter.y() - curPos.y());
                break;
            default:
                distance = -1;
                offset = -1;
            }

            if (distance > 0) {
                // Inverse score
                int score = distance + offset + ((offset * offset) / distance);
                if (score < bestScore || !switchTo) {
                    switchTo = other;
                    bestScore = score;
                }
            }
        }
    }
    if (switchTo) {
        activateWindow(switchTo);
    }

    return switchTo;
}

/**
 * Shows the window operations popup menu for the active window.
 */
void Workspace::slotWindowOperations()
{
    if (!m_activeWindow) {
        return;
    }
    QPoint pos = m_activeWindow->pos().toPoint() + m_activeWindow->clientPos().toPoint();
    showWindowMenu(QRect(pos, pos), m_activeWindow);
}

void Workspace::showWindowMenu(const QRect &pos, Window *window)
{
    m_userActionsMenu->show(pos, window);
}

void Workspace::showApplicationMenu(const QRect &pos, Window *window, int actionId)
{
    ApplicationMenu::self()->showApplicationMenu(window->pos().toPoint() + pos.bottomLeft(), window, actionId);
}

/**
 * Closes the active window.
 */
void Workspace::slotWindowClose()
{
    // TODO: why?
    //     if ( tab_box->isVisible())
    //         return;
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::CloseOp);
    }
}

/**
 * Starts keyboard move mode for the active window.
 */
void Workspace::slotWindowMove()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::UnrestrictedMoveOp);
    }
}

/**
 * Starts keyboard resize mode for the active window.
 */
void Workspace::slotWindowResize()
{
    if (USABLE_ACTIVE_WINDOW) {
        performWindowOperation(m_activeWindow, Options::UnrestrictedResizeOp);
    }
}

#undef USABLE_ACTIVE_WINDOW

void Window::setShortcut(const QString &_cut)
{
    QString cut = rules()->checkShortcut(_cut);
    auto updateShortcut = [this](const QKeySequence &cut = QKeySequence()) {
        if (_shortcut == cut) {
            return;
        }
        _shortcut = cut;
        setShortcutInternal();
    };
    if (cut.isEmpty()) {
        updateShortcut();
        return;
    }
    if (cut == shortcut().toString()) {
        return; // no change
    }
    // Format:
    // base+(abcdef)<space>base+(abcdef)
    // E.g. Alt+Ctrl+(ABCDEF);Meta+X,Meta+(ABCDEF)
    if (!cut.contains(QLatin1Char('(')) && !cut.contains(QLatin1Char(')')) && !cut.contains(QLatin1String(" - "))) {
        if (workspace()->shortcutAvailable(cut, this)) {
            updateShortcut(QKeySequence(cut));
        } else {
            updateShortcut();
        }
        return;
    }
    const QRegularExpression reg(QStringLiteral("(.*\\+)\\((.*)\\)"));
    QList<QKeySequence> keys;
    QStringList groups = cut.split(QStringLiteral(" - "));
    for (QStringList::ConstIterator it = groups.constBegin();
         it != groups.constEnd();
         ++it) {
        const QRegularExpressionMatch match = reg.match(*it);
        if (match.hasMatch()) {
            const QString base = match.captured(1);
            const QString list = match.captured(2);
            for (int i = 0;
                 i < list.length();
                 ++i) {
                QKeySequence c(base + list[i]);
                if (!c.isEmpty()) {
                    keys.append(c);
                }
            }
        } else {
            // regexp doesn't match, so it should be a normal shortcut
            QKeySequence c(*it);
            if (!c.isEmpty()) {
                keys.append(c);
            }
        }
    }
    for (auto it = keys.constBegin();
         it != keys.constEnd();
         ++it) {
        if (_shortcut == *it) { // current one is in the list
            return;
        }
    }
    for (auto it = keys.constBegin();
         it != keys.constEnd();
         ++it) {
        if (workspace()->shortcutAvailable(*it, this)) {
            updateShortcut(*it);
            return;
        }
    }
    updateShortcut();
}

void Window::setShortcutInternal()
{
    updateCaption();
    workspace()->windowShortcutUpdated(this);
}

void X11Window::setShortcutInternal()
{
    updateCaption();
#if 0
    workspace()->windowShortcutUpdated(this);
#else
    // Workaround for kwin<->kglobalaccel deadlock, when KWin has X grab and the kded
    // kglobalaccel module tries to create the key grab. KWin should preferably grab
    // they keys itself anyway :(.
    QTimer::singleShot(0, this, std::bind(&Workspace::windowShortcutUpdated, workspace(), this));
#endif
}

bool Workspace::shortcutAvailable(const QKeySequence &cut, Window *ignore) const
{
    if (ignore && cut == ignore->shortcut()) {
        return true;
    }

    // Check if the shortcut is already registered
    const QList<KGlobalShortcutInfo> registeredShortcuts = KGlobalAccel::globalShortcutsByKey(cut);
    for (const auto &shortcut : registeredShortcuts) {
        // Only return "not available" if is not a window activation shortcut, as it may be no longer valid
        if (!shortcut.uniqueName().startsWith(QStringLiteral("_k_session:"))) {
            return false;
        }
    }
    // Check now conflicts with activation shortcuts for current windows
    for (const auto window : qAsConst(m_allClients)) {
        if (window != ignore && window->shortcut() == cut) {
            return false;
        }
    }
    return true;
}

} // namespace
