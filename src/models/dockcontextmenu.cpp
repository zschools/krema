// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Krema Contributors

#include "dockcontextmenu.h"

#include "desktopactions.h"
#include "dockactions.h"
#include "dockmodel.h"
#include "krema.h"
#include "notificationtracker.h"

#include <taskmanager/abstracttasksmodel.h>
#include <taskmanager/tasksmodel.h>

#include <KLocalizedString>

#include <QApplication>
#include <QCursor>
#include <QIcon>
#include <QMenu>
#include <QMouseEvent>
#include <QWindow>

namespace krema
{

DockContextMenu::DockContextMenu(DockModel *model, DockActions *actions, NotificationTracker *tracker, KremaSettings *settings, QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_actions(actions)
    , m_tracker(tracker)
    , m_settings(settings)
{
}

void DockContextMenu::showForTask(int index)
{
    auto *tasksModel = m_model->tasksModel();
    const QModelIndex idx = tasksModel->index(index, 0);
    if (!idx.isValid()) {
        return;
    }

    const bool isWindow = idx.data(TaskManager::AbstractTasksModel::IsWindow).toBool();

    const bool pinned = m_model->isPinned(index);

    auto *menu = new QMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);

    // No app-name header: the menu starts directly with the app's
    // desktop entry actions.
    // Desktop entry actions (e.g. "New Private Window"), placed like Plasma's
    // task manager. Group parents report their first window's launcher URL.
    // Each action is captured by value together with its KService, so it
    // launches the right application even if task rows change while the menu
    // is open.
    const QList<KServiceAction> appActions = desktopActions(idx.data(TaskManager::AbstractTasksModel::LauncherUrlWithoutIcon).toUrl());
    if (!appActions.isEmpty()) {
        for (const KServiceAction &action : appActions) {
            if (action.isSeparator()) {
                menu->addSeparator();
                continue;
            }
            // Desktop files use plain text; '&' is not a mnemonic marker.
            QString text = action.text();
            text.replace(QLatin1Char('&'), QLatin1String("&&"));
            menu->addAction(QIcon::fromTheme(action.icon()), text, this, [action]() {
                launchDesktopAction(action);
            });
        }
        menu->addSeparator();
    }

    // Pin / Unpin
    if (pinned) {
        menu->addAction(QIcon::fromTheme(QStringLiteral("window-unpin")), i18nc("@action:inmenu", "Unpin from Dock"), this, [this, index]() {
            m_actions->togglePinned(index);
        });
    } else {
        menu->addAction(QIcon::fromTheme(QStringLiteral("window-pin")), i18nc("@action:inmenu", "Pin to Dock"), this, [this, index]() {
            m_actions->togglePinned(index);
        });
    }

    // New Instance
    menu->addAction(QIcon::fromTheme(QStringLiteral("list-add")), i18nc("@action:inmenu", "New Instance"), this, [this, index]() {
        m_actions->newInstance(index);
    });

    // Clear Notifications (only when badges are present)
    if (m_tracker != nullptr) {
        const QString appId = m_model->appId(index);
        if (!appId.isEmpty() && m_tracker->unreadCount(appId) > 0) {
            menu->addAction(QIcon::fromTheme(QStringLiteral("edit-clear-history")), i18nc("@action:inmenu", "Clear Notifications"), this, [this, appId]() {
                m_tracker->clearUnreadNotifications(appId);
            });
        }
    }

    // About / Quit come first in the footer: they occupy the slot Close
    // used to have, right after the task actions. Settings follows, and
    // Close is the very last item in the menu.
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme(QStringLiteral("help-about")), i18nc("@action:inmenu", "About Krema"), this, [this]() {
        Q_EMIT aboutRequested();
    });
    menu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")), i18nc("@action:inmenu", "Quit"), qApp, &QApplication::quit);

    // Settings
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme(QStringLiteral("configure")), i18nc("@action:inmenu", "Settings..."), this, [this]() {
        Q_EMIT settingsRequested();
    });

    // Close (only for running windows), kept last in the menu. A task
    // with several windows first lists each window's title: activating an
    // entry raises that window, and the close action becomes Close All.
    if (isWindow) {
        menu->addSeparator();
        const int childCount = tasksModel->rowCount(idx);
        if (childCount > 1) {
            const int maxLength = m_settings != nullptr ? m_settings->windowTitleMaxLength() : 60;
            for (int child = 0; child < childCount; ++child) {
                const QModelIndex childIdx = tasksModel->index(child, 0, idx);
                QString title = childIdx.data(Qt::DisplayRole).toString();
                if (maxLength > 0 && title.length() > maxLength) {
                    title = title.left(maxLength) + QStringLiteral("…");
                }
                menu->addAction(childIdx.data(Qt::DecorationRole).value<QIcon>(), title, this, [this, index, child]() {
                    m_actions->activateChildWindow(index, child);
                });
            }
            menu->addSeparator();
            menu->addAction(QIcon::fromTheme(QStringLiteral("window-close")), i18nc("@action:inmenu", "Close All"), this, [this, index]() {
                m_actions->closeAllTasks(index);
            });
        } else {
            menu->addAction(QIcon::fromTheme(QStringLiteral("window-close")), i18nc("@action:inmenu", "Close"), this, [this, index]() {
                m_actions->closeTask(index);
            });
        }
    }

    m_currentIndex = index;
    m_switchArmed = false;

    // Open before popup(): the popup grab makes QtWayland send the dock a
    // synthetic leave, which QML must already see as part of the menu hold.
    // Also drives the interaction lock (dock stays visible while menu is open).
    setOpen(true);
    m_activeMenu = menu;
    // While the menu holds the popup grab, a click outside it is consumed
    // by Qt just to dismiss the menu and never reaches the dock. Watch for
    // a right-click on another dock icon so the menu can switch straight
    // to that icon instead of making the user click twice (eventFilter).
    qApp->installEventFilter(this);
    connect(menu, &QMenu::aboutToHide, this, [this]() {
        setOpen(false);
        qApp->removeEventFilter(this);
        m_activeMenu = nullptr;
        if (m_switchArmed) {
            m_switchArmed = false;
            const QPointF point = m_switchPoint;
            const int previousIndex = m_currentIndex;
            // Queued: the new menu must only pop up once this menu's grab
            // has been fully released.
            QMetaObject::invokeMethod(this, [this, point, previousIndex]() {
                Q_EMIT switchRequested(point.x(), point.y(), previousIndex);
            }, Qt::QueuedConnection);
        }
    });

    menu->popup(QCursor::pos());
}

void DockContextMenu::setDockWindow(QWindow *window)
{
    m_dockWindow = window;
}

bool DockContextMenu::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress && m_activeMenu && m_dockWindow) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::RightButton) {
            const QPoint globalPos = mouseEvent->globalPosition().toPoint();
            const bool insideMenu = m_activeMenu->geometry().contains(globalPos);
            if (!insideMenu && m_dockWindow->geometry().contains(globalPos)) {
                m_switchPoint = QPointF(globalPos - m_dockWindow->geometry().topLeft());
                m_switchArmed = true;
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

bool DockContextMenu::isOpen() const
{
    return m_open;
}

void DockContextMenu::setOpen(bool open)
{
    if (m_open == open) {
        return;
    }
    m_open = open;
    Q_EMIT openChanged();
}

} // namespace krema
