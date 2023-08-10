/*
 * Copyright (C) 2023 Jan Grulich <jgrulich@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#include "qadwaitadecoration.h"

#include <QtWaylandClient/private/qwaylandshellsurface_p.h>
#include <QtWaylandClient/private/qwaylandshmbackingstore_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>
#include <QtWaylandClient/private/wayland-wayland-client-protocol.h>

#include <qpa/qwindowsysteminterface.h>

#include <QtGui/QColor>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>

#include <QtCore/QVariant>

// QtDBus
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCall>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>
#include <QtDBus/QDBusVariant>
#include <QtDBus/QtDBus>

static constexpr int ceButtonSpacing = 14;
static constexpr int ceButtonWidth = 24;
static constexpr int ceShadowsWidth = 10;
static constexpr int ceTitlebarHeight = 38;
static constexpr int ceWindowBorderWidth = 1;

const QDBusArgument &operator>>(const QDBusArgument &argument, QMap<QString, QVariantMap> &map)
{
    argument.beginMap();
    map.clear();

    while (!argument.atEnd()) {
        QString key;
        QVariantMap value;
        argument.beginMapEntry();
        argument >> key >> value;
        argument.endMapEntry();
        map.insert(key, value);
    }

    argument.endMap();
    return argument;
}

QAdwaitaDecoration::QAdwaitaDecoration()
{
    m_lastButtonClick = QDateTime::currentDateTime();

    QTextOption option(Qt::AlignHCenter | Qt::AlignVCenter);
    option.setWrapMode(QTextOption::NoWrap);
    m_windowTitle.setTextOption(option);

    QTimer::singleShot(0, this, &QAdwaitaDecoration::initTitlebarLayout);
}

void QAdwaitaDecoration::initTitlebarLayout()
{
    qDBusRegisterMetaType<QMap<QString, QVariantMap>>();

    // TODO: title-bar-font, double-click-interval
    QDBusMessage message = QDBusMessage::createMethodCall(
            QLatin1String("org.freedesktop.portal.Desktop"),
            QLatin1String("/org/freedesktop/portal/desktop"),
            QLatin1String("org.freedesktop.portal.Settings"), QLatin1String("ReadAll"));
    message << QStringList{ { QLatin1String("org.gnome.desktop.wm.preferences") } };
    QDBusPendingCall pendingCall = QDBusConnection::sessionBus().asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall);
    QObject::connect(
            watcher, &QDBusPendingCallWatcher::finished, [=](QDBusPendingCallWatcher *watcher) {
                QDBusPendingReply<QMap<QString, QVariantMap>> reply = *watcher;
                if (reply.isValid()) {
                    QMap<QString, QVariantMap> settings = reply.value();
                    const QString buttonLayout =
                            settings.value(QLatin1String("org.gnome.desktop.wm.preferences"))
                                    .value(QLatin1String("button-layout"))
                                    .toString();
                    updateTitlebarLayout(buttonLayout);
                    watcher->deleteLater();
                }
            });

    QDBusConnection::sessionBus().connect(
            QString(), QLatin1String("/org/freedesktop/portal/desktop"),
            QLatin1String("org.freedesktop.portal.Settings"), QLatin1String("SettingChanged"), this,
            SLOT(settingChanged(QString, QString, QDBusVariant)));
}

void QAdwaitaDecoration::updateTitlebarLayout(const QString &layout)
{
    const QStringList btnList = layout.split(QLatin1Char(':'));
    if (btnList.count() == 2) {
        const QString &leftButtons = btnList.first();
        m_placement = leftButtons.contains(QLatin1String("close")) ? Left : Right;
    }

    Buttons buttons;
    if (layout.contains(QLatin1String("close")))
        buttons = buttons | Close;
    if (layout.contains(QLatin1String("maximize")))
        buttons = buttons | Maximize;
    if (layout.contains(QLatin1String("minimize")))
        buttons = buttons | Minimize;

    m_buttons = buttons;

    forceRepaint();
}

void QAdwaitaDecoration::settingChanged(const QString &group, const QString &key,
                                        const QDBusVariant &value)
{
    if (group == QLatin1String("org.gnome.desktop.wm.preferences")
        && key == QLatin1String("button-layout")) {
        const QString layout = value.variant().toString();
        updateTitlebarLayout(layout);
    }
}

QRectF QAdwaitaDecoration::buttonRect(Button button) const
{
    const int minimizeButtonPosition = m_buttons.testFlag(Maximize) ? 3 : 2;
    const int buttonPosition = button == Close ? 1
            : button == Maximize               ? 2
                                               : minimizeButtonPosition;

    int xPos;
    int yPos;

    if (m_placement == Right) {
        xPos = windowContentGeometry().width();
        xPos -= ceButtonWidth * buttonPosition;
        xPos -= ceButtonSpacing * buttonPosition;
        xPos -= margins().right();
    } else {
        xPos = 0;
        xPos += ceButtonWidth * buttonPosition;
        xPos += ceButtonSpacing * buttonPosition;
        xPos += margins().left();
    }

    yPos = margins().top();
    yPos += margins().bottom();
    yPos -= ceButtonWidth;
    yPos /= 2;

    return QRectF(xPos, yPos, ceButtonWidth, ceButtonWidth);
}

QMargins QAdwaitaDecoration::margins(MarginsType marginsType) const
{
    const bool maximized = waylandWindow()->windowStates() & Qt::WindowMaximized;
    const bool tiledLeft =
            waylandWindow()->toplevelWindowTilingStates() & QWaylandWindow::WindowTiledLeft;
    const bool tiledRight =
            waylandWindow()->toplevelWindowTilingStates() & QWaylandWindow::WindowTiledRight;
    const bool tiledTop =
            waylandWindow()->toplevelWindowTilingStates() & QWaylandWindow::WindowTiledTop;
    const bool tiledBottom =
            waylandWindow()->toplevelWindowTilingStates() & QWaylandWindow::WindowTiledBottom;

    if (maximized) {
        // Maximized windows don't have anything around, no shadows, border,
        // etc. Only report titlebar height in case we are not asking for shadow
        // margins.
        return QMargins(0, marginsType == ShadowsOnly ? 0 : ceTitlebarHeight, 0, 0);
    }

    // Since all sides (left, right, bottom) are going to be same
    const int marginsCommon = marginsType == ShadowsExcluded ? ceWindowBorderWidth
                                                             : ceShadowsWidth + ceWindowBorderWidth;
    const int sideMargins = marginsType == ShadowsOnly ? ceShadowsWidth : marginsCommon;
    const int topMargins =
            marginsType == ShadowsOnly ? ceShadowsWidth : ceTitlebarHeight + marginsCommon;

    return QMargins(tiledLeft ? 0 : sideMargins,
                    tiledTop ? marginsType == ShadowsOnly ? 0 : ceTitlebarHeight : topMargins,
                    tiledRight ? 0 : sideMargins, tiledBottom ? 0 : sideMargins);
}

void QAdwaitaDecoration::paint(QPaintDevice *device)
{
    const Qt::WindowStates windowStates = waylandWindow()->windowStates();
    const bool active = windowStates & Qt::WindowActive;

    const bool maximized = windowStates & Qt::WindowMaximized;
    const bool tiledLeft =
            waylandWindow()->toplevelWindowTilingStates() & QWaylandWindow::WindowTiledLeft;
    const bool tiledRight =
            waylandWindow()->toplevelWindowTilingStates() & QWaylandWindow::WindowTiledRight;
    const bool tiledTop =
            waylandWindow()->toplevelWindowTilingStates() & QWaylandWindow::WindowTiledTop;
    const bool tiledBottom =
            waylandWindow()->toplevelWindowTilingStates() & QWaylandWindow::WindowTiledBottom;

    const QRect surfaceRect = windowContentGeometry();
    // TODO
    const QColor borderColor = active ? Qt::blue : Qt::darkBlue;

    QPainter p(device);
    p.setRenderHint(QPainter::Antialiasing);

    // Titlebar
    {
        QPainterPath path;
        const int titleBarWidth = surfaceRect.width() - margins().left() - margins().right();
        const int borderRectHeight = surfaceRect.height() - margins().top() - margins().bottom();

        if (maximized || tiledRight || tiledLeft)
            path.addRect(margins().left(), margins().bottom(), titleBarWidth, margins().top());
        else
            path.addRoundedRect(margins().left(), margins().bottom(), titleBarWidth,
                                margins().top(), 8, 8);

        p.save();
        p.setPen(Qt::blue);
        p.fillPath(path.simplified(), Qt::white);
        p.drawPath(path);
        p.drawRect(margins().left(), margins().top(), titleBarWidth, borderRectHeight);
        p.restore();
    }

    // Window title
    {
        const QRect top = QRect(margins().left(), margins().bottom(), surfaceRect.width(),
                                margins().top() - margins().bottom());
        const QString windowTitleText = window()->title();
        if (!windowTitleText.isEmpty()) {
            if (m_windowTitle.text() != windowTitleText) {
                m_windowTitle.setText(windowTitleText);
                m_windowTitle.prepare();
            }

            QRect titleBar = top;
            if (m_placement == Right) {
                titleBar.setLeft(margins().left());
                titleBar.setRight(static_cast<int>(buttonRect(Minimize).left()) - 8);
            } else {
                titleBar.setLeft(static_cast<int>(buttonRect(Minimize).right()) + 8);
                titleBar.setRight(surfaceRect.width() - margins().right());
            }

            p.save();
            p.setClipRect(titleBar);
            // TODO
            p.setPen(Qt::black);
            // p.setPen(active ? m_foregroundColor : m_foregroundInactiveColor);
            QSizeF size = m_windowTitle.size();
            int dx = (static_cast<int>(top.width()) - static_cast<int>(size.width())) / 2;
            int dy = (static_cast<int>(top.height()) - static_cast<int>(size.height())) / 2;
            // TODO
            QFont font;
            // const QFont *themeFont = font(QPlatformTheme::TitleBarFont);
            // font.setPointSizeF(themeFont->pointSizeF());
            // font.setFamily(themeFont->family());
            // font.setBold(themeFont->bold());
            // p.setFont(font);
            QPoint windowTitlePoint(top.topLeft().x() + dx, top.topLeft().y() + dy);
            p.drawStaticText(windowTitlePoint, m_windowTitle);
            p.restore();
        }
    }

    // Buttons
    {
        if (m_buttons.testFlag(Close))
            paintButton(Close, &p);

        if (m_buttons.testFlag(Maximize))
            paintButton(Maximize, &p);

        if (m_buttons.testFlag(Minimize))
            paintButton(Minimize, &p);
    }
}

static void renderFlatRoundedButtonFrame(QAdwaitaDecoration::Button button, QPainter *painter,
                                         const QRect &rect, const QColor &color)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawEllipse(rect);
    painter->restore();
}

static void renderButtonIcon(QAdwaitaDecoration::Button button, QPainter *painter, bool maximized,
                             const QRect &rect, const QColor &color)
{
    painter->save();
    painter->setViewport(rect);
    painter->setWindow(0, 0, ceButtonWidth, ceButtonWidth);
    painter->setRenderHints(QPainter::Antialiasing, false);

    QPen pen;
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::MiterJoin);
    pen.setColor(color);

    if (button == QAdwaitaDecoration::Close) {
        painter->setRenderHints(QPainter::Antialiasing, true);
        painter->setBrush(Qt::white);
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(6, 6, 12, 12);

        painter->setRenderHints(QPainter::Antialiasing, false);
        painter->setPen(pen);
        painter->drawLine(QPointF(9.5, 9.5), QPointF(14.5, 14.5));
        painter->drawLine(QPointF(9.5, 14.5), QPointF(14.5, 9));
    } else {
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);

        if (button == QAdwaitaDecoration::Maximize) {
            painter->drawLine(QPointF(5.5, 13.5), QPointF(11.5, 7.5));
            painter->drawLine(QPointF(12, 8), QPointF(18, 14));
        } else {
            painter->drawLine(QPointF(5.5, 9.5), QPointF(11.5, 15.5));
            painter->drawLine(QPointF(12, 15), QPointF(18, 9));
        }
    }

    painter->restore();
}

void QAdwaitaDecoration::paintButton(Button button, QPainter *painter)
{
    const Qt::WindowStates windowStates = waylandWindow()->windowStates();
    const bool active = windowStates & Qt::WindowActive;
    const bool maximized = windowStates & Qt::WindowMaximized;

    const QRect btnRect = buttonRect(button).toRect();
    renderFlatRoundedButtonFrame(button, painter, btnRect,
                                 m_hoveredButtons.testFlag(button) ? Qt::red : Qt::darkRed);
    renderButtonIcon(button, painter, maximized, btnRect, Qt::black);
}

bool QAdwaitaDecoration::clickButton(Qt::MouseButtons b, Button btn)
{
    if (isLeftClicked(b)) {
        m_clicking = btn;
        return false;
    } else if (isLeftReleased(b)) {
        if (m_clicking == btn) {
            m_clicking = None;
            return true;
        } else {
            m_clicking = None;
        }
    }
    return false;
}

bool QAdwaitaDecoration::doubleClickButton(Qt::MouseButtons b, const QPointF &local,
                                           const QDateTime &currentTime)
{
    if (b & Qt::LeftButton) {
        const qint64 clickInterval = m_lastButtonClick.msecsTo(currentTime);
        m_lastButtonClick = currentTime;
        // TODO
        const int doubleClickDistance = 5;
        const QPointF posDiff = m_lastButtonClickPosition - local;
        if ((clickInterval <= 500)
            && ((posDiff.x() <= doubleClickDistance && posDiff.x() >= -doubleClickDistance)
                && ((posDiff.y() <= doubleClickDistance && posDiff.y() >= -doubleClickDistance)))) {
            return true;
        }

        m_lastButtonClickPosition = local;
    }

    return false;
}

bool QAdwaitaDecoration::handleMouse(QWaylandInputDevice *inputDevice, const QPointF &local,
                                     const QPointF &global, Qt::MouseButtons b,
                                     Qt::KeyboardModifiers mods)
{
    Q_UNUSED(global)

    if (local.y() > margins().top()) {
        updateButtonHoverState(Button::None);
    }

    // Figure out what area mouse is in
    QRect surfaceRect = windowContentGeometry();
    if (local.y() <= surfaceRect.top() + margins().top()) {
        processMouseTop(inputDevice, local, b, mods);
    } else if (local.y() > surfaceRect.bottom() - margins().bottom()) {
        processMouseBottom(inputDevice, local, b, mods);
    } else if (local.x() <= surfaceRect.left() + margins().left()) {
        processMouseLeft(inputDevice, local, b, mods);
    } else if (local.x() > surfaceRect.right() - margins().right()) {
        processMouseRight(inputDevice, local, b, mods);
    } else {
#if QT_CONFIG(cursor)
        waylandWindow()->restoreMouseCursor(inputDevice);
#endif
        setMouseButtons(b);
        return false;
    }

    setMouseButtons(b);
    return true;
}

#if QT_VERSION >= 0x060000
bool QAdwaitaDecoration::handleTouch(QWaylandInputDevice *inputDevice, const QPointF &local,
                                     const QPointF &global, QEventPoint::State state,
                                     Qt::KeyboardModifiers mods)
#else
bool QAdwaitaDecoration::handleTouch(QWaylandInputDevice *inputDevice, const QPointF &local,
                                     const QPointF &global, Qt::TouchPointState state,
                                     Qt::KeyboardModifiers mods)
#endif
{
    Q_UNUSED(inputDevice)
    Q_UNUSED(global)
    Q_UNUSED(mods)
#if QT_VERSION >= 0x060000
    bool handled = state == QEventPoint::Pressed;
#else
    bool handled = state == Qt::TouchPointPressed;
#endif
    if (handled) {
        if (buttonRect(Close).contains(local)) {
            QWindowSystemInterface::handleCloseEvent(window());
        } else if (m_buttons.testFlag(Maximize) && buttonRect(Maximize).contains(local)) {
            window()->setWindowStates(window()->windowStates() ^ Qt::WindowMaximized);
        } else if (m_buttons.testFlag(Minimize) && buttonRect(Minimize).contains(local)) {
            window()->setWindowState(Qt::WindowMinimized);
        } else if (local.y() <= margins().top()) {
            waylandWindow()->shellSurface()->move(inputDevice);
        } else {
            handled = false;
        }
    }

    return handled;
}

QRect QAdwaitaDecoration::windowContentGeometry() const
{
    return waylandWindow()->windowContentGeometry() + margins(ShadowsOnly);
}

void QAdwaitaDecoration::loadConfiguration()
{
    // TODO
}

void QAdwaitaDecoration::forceRepaint()
{
    // Set dirty flag
    waylandWindow()->decoration()->update();
    // Force re-paint
    // NOTE: not sure it's correct, but it's the only way to make it work
    if (waylandWindow()->backingStore()) {
        waylandWindow()->backingStore()->flush(window(), QRegion(), QPoint());
    }
}

void QAdwaitaDecoration::processMouseTop(QWaylandInputDevice *inputDevice, const QPointF &local,
                                         Qt::MouseButtons b, Qt::KeyboardModifiers mods)
{
    Q_UNUSED(mods)

    QDateTime currentDateTime = QDateTime::currentDateTime();
    QRect surfaceRect = windowContentGeometry();

    if (!buttonRect(Close).contains(local) && !buttonRect(Maximize).contains(local)
        && !buttonRect(Minimize).contains(local)) {
        updateButtonHoverState(Button::None);
    }

    if (local.y() <= surfaceRect.top() + margins().bottom()) {
        if (local.x() <= margins().left()) {
            // top left bit
#if QT_CONFIG(cursor)
            waylandWindow()->setMouseCursor(inputDevice, Qt::SizeFDiagCursor);
#endif
            startResize(inputDevice, Qt::TopEdge | Qt::LeftEdge, b);
        } else if (local.x() > surfaceRect.right() - margins().left()) {
            // top right bit
#if QT_CONFIG(cursor)
            waylandWindow()->setMouseCursor(inputDevice, Qt::SizeBDiagCursor);
#endif
            startResize(inputDevice, Qt::TopEdge | Qt::RightEdge, b);
        } else {
            // top resize bit
#if QT_CONFIG(cursor)
            waylandWindow()->setMouseCursor(inputDevice, Qt::SplitVCursor);
#endif
            startResize(inputDevice, Qt::TopEdge, b);
        }
    } else if (local.x() <= surfaceRect.left() + margins().left()) {
        processMouseLeft(inputDevice, local, b, mods);
    } else if (local.x() > surfaceRect.right() - margins().right()) {
        processMouseRight(inputDevice, local, b, mods);
    } else if (buttonRect(Close).contains(local)) {
        if (clickButton(b, Close)) {
            QWindowSystemInterface::handleCloseEvent(window());
            m_hoveredButtons.setFlag(Close, false);
        }
        updateButtonHoverState(Close);
    } else if (m_buttons.testFlag(Maximize) && buttonRect(Maximize).contains(local)) {
        updateButtonHoverState(Maximize);
        if (clickButton(b, Maximize)) {
            window()->setWindowStates(window()->windowStates() ^ Qt::WindowMaximized);
            m_hoveredButtons.setFlag(Maximize, false);
        }
    } else if (m_buttons.testFlag(Minimize) && buttonRect(Minimize).contains(local)) {
        updateButtonHoverState(Minimize);
        if (clickButton(b, Minimize)) {
            window()->setWindowState(Qt::WindowMinimized);
            m_hoveredButtons.setFlag(Minimize, false);
        }
    } else if (doubleClickButton(b, local, currentDateTime)) {
        window()->setWindowStates(window()->windowStates() ^ Qt::WindowMaximized);
    } else {
        // Show window menu
        if (b == Qt::MouseButton::RightButton) {
            waylandWindow()->shellSurface()->showWindowMenu(inputDevice);
        }
#if QT_CONFIG(cursor)
        waylandWindow()->restoreMouseCursor(inputDevice);
#endif
        startMove(inputDevice, b);
    }
}

void QAdwaitaDecoration::processMouseBottom(QWaylandInputDevice *inputDevice, const QPointF &local,
                                            Qt::MouseButtons b, Qt::KeyboardModifiers mods)
{
    Q_UNUSED(mods)
    if (local.x() <= margins().left()) {
        // bottom left bit
#if QT_CONFIG(cursor)
        waylandWindow()->setMouseCursor(inputDevice, Qt::SizeBDiagCursor);
#endif
        startResize(inputDevice, Qt::BottomEdge | Qt::LeftEdge, b);
    } else if (local.x() > window()->width() + margins().right()) {
        // bottom right bit
#if QT_CONFIG(cursor)
        waylandWindow()->setMouseCursor(inputDevice, Qt::SizeFDiagCursor);
#endif
        startResize(inputDevice, Qt::BottomEdge | Qt::RightEdge, b);
    } else {
        // bottom bit
#if QT_CONFIG(cursor)
        waylandWindow()->setMouseCursor(inputDevice, Qt::SplitVCursor);
#endif
        startResize(inputDevice, Qt::BottomEdge, b);
    }
}

void QAdwaitaDecoration::processMouseLeft(QWaylandInputDevice *inputDevice, const QPointF &local,
                                          Qt::MouseButtons b, Qt::KeyboardModifiers mods)
{
    Q_UNUSED(local)
    Q_UNUSED(mods)
#if QT_CONFIG(cursor)
    waylandWindow()->setMouseCursor(inputDevice, Qt::SplitHCursor);
#endif
    startResize(inputDevice, Qt::LeftEdge, b);
}

void QAdwaitaDecoration::processMouseRight(QWaylandInputDevice *inputDevice, const QPointF &local,
                                           Qt::MouseButtons b, Qt::KeyboardModifiers mods)
{
    Q_UNUSED(local)
    Q_UNUSED(mods)
#if QT_CONFIG(cursor)
    waylandWindow()->setMouseCursor(inputDevice, Qt::SplitHCursor);
#endif
    startResize(inputDevice, Qt::RightEdge, b);
}

bool QAdwaitaDecoration::updateButtonHoverState(Button hoveredButton)
{
    bool currentCloseButtonState = m_hoveredButtons.testFlag(Close);
    bool currentMaximizeButtonState = m_hoveredButtons.testFlag(Maximize);
    bool currentMinimizeButtonState = m_hoveredButtons.testFlag(Minimize);

    m_hoveredButtons.setFlag(Close, hoveredButton == Button::Close);
    m_hoveredButtons.setFlag(Maximize, hoveredButton == Button::Maximize);
    m_hoveredButtons.setFlag(Minimize, hoveredButton == Button::Minimize);

    if (m_hoveredButtons.testFlag(Close) != currentCloseButtonState
        || m_hoveredButtons.testFlag(Maximize) != currentMaximizeButtonState
        || m_hoveredButtons.testFlag(Minimize) != currentMinimizeButtonState) {
        forceRepaint();
        return true;
    }

    return false;
}
