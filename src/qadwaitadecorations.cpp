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

#include "qadwaitadecorations.h"

#include <QtWaylandClient/private/qwaylandshellsurface_p.h>
#include <QtWaylandClient/private/qwaylandshmbackingstore_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>

#include <QtCore/QLoggingCategory>
#include <QScopeGuard>

#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/qpa/qplatformtheme.h>

#include <QtSvg/QSvgRenderer>

// QtDBus
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCall>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>
#include <QtDBus/QDBusVariant>
#include <QtDBus/QtDBus>

static constexpr int ceButtonSpacing = 12;
static constexpr int ceButtonWidth = 24;
static constexpr int ceCornerRadius = 12;
static constexpr int ceShadowsWidth = 10;
static constexpr int ceTitlebarHeight = 38;
static constexpr int ceWindowBorderWidth = 1;
static constexpr qreal ceTitlebarSeperatorWidth = 0.5;

static QMap<QAdwaitaDecorations::ButtonIcon, QString> buttonMap = {
    { QAdwaitaDecorations::CloseIcon, QStringLiteral("window-close-symbolic") },
    { QAdwaitaDecorations::MinimizeIcon, QStringLiteral("window-minimize-symbolic") },
    { QAdwaitaDecorations::MaximizeIcon, QStringLiteral("window-maximize-symbolic") },
    { QAdwaitaDecorations::RestoreIcon, QStringLiteral("window-restore-symbolic") }
};

Q_DECL_IMPORT void qt_blurImage(QPainter *p, QImage &blurImage, qreal radius, bool quality,
                                bool alphaOnly, int transposed = 0);

Q_LOGGING_CATEGORY(QAdwaitaDecorationsLog, "qt.qpa.qadwaitadecorations", QtWarningMsg)

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

QAdwaitaDecorations::QAdwaitaDecorations()
{
#ifdef HAS_QT6_SUPPORT
#  if QT_VERSION >= 0x060000
    qCDebug(QAdwaitaDecorationsLog) << "Using Qt6 version";
#  else
    qCDebug(QAdwaitaDecorationsLog) << "Using Qt5 version with Qt6 backports";
#  endif
#else
    qCDebug(QAdwaitaDecorationsLog) << "Using Qt5 version";
#endif

    m_lastButtonClick = QDateTime::currentDateTime();

    QTextOption option(Qt::AlignHCenter | Qt::AlignVCenter);
    option.setWrapMode(QTextOption::NoWrap);
    m_windowTitle.setTextOption(option);
    m_windowTitle.setTextFormat(Qt::PlainText);

    const QPlatformTheme *theme = QGuiApplicationPrivate::platformTheme();
    if (const QFont *font = theme->font(QPlatformTheme::TitleBarFont))
        m_font = std::make_unique<QFont>(*font);
    if (!m_font)
        m_font = std::make_unique<QFont>(QLatin1String("Sans"), 10);

    QTimer::singleShot(0, this, &QAdwaitaDecorations::initConfiguration);
}

void QAdwaitaDecorations::initConfiguration()
{
    qRegisterMetaType<QDBusVariant>();
    qDBusRegisterMetaType<QMap<QString, QVariantMap>>();

    QDBusConnection connection = QDBusConnection::sessionBus();

    QDBusMessage message = QDBusMessage::createMethodCall(
            QLatin1String("org.freedesktop.portal.Desktop"),
            QLatin1String("/org/freedesktop/portal/desktop"),
            QLatin1String("org.freedesktop.portal.Settings"), QLatin1String("ReadAll"));
    message << QStringList{ { QLatin1String("org.gnome.desktop.wm.preferences") },
                            { QLatin1String("org.freedesktop.appearance") } };

    QDBusPendingCall pendingCall = connection.asyncCall(message);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(pendingCall);
    QObject::connect(
            watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *watcher) {
                QDBusPendingReply<QMap<QString, QVariantMap>> reply = *watcher;
                if (reply.isValid()) {
                    QMap<QString, QVariantMap> settings = reply.value();
                    if (!settings.isEmpty()) {
                        const uint colorScheme =
                                settings.value(QLatin1String("org.freedesktop.appearance"))
                                        .value(QLatin1String("color-scheme"))
                                        .toUInt();
                        updateColors(colorScheme == 1); // 1 == Prefer Dark
                        const QString buttonLayout =
                                settings.value(QLatin1String("org.gnome.desktop.wm.preferences"))
                                        .value(QLatin1String("button-layout"))
                                        .toString();
                        if (!buttonLayout.isEmpty()) {
                            updateTitlebarLayout(buttonLayout);
                        }
                        // Workaround for QGtkStyle not having correct titlebar font
                        // This is not going to be very precise as I want to avoid dependency on
                        // Pango which we had in QGnomePlatform, but at least make the font bold
                        // if detected.
                        const QString titlebarFont =
                                settings.value(QLatin1String("org.gnome.desktop.wm.preferences"))
                                        .value(QLatin1String("titlebar-font"))
                                        .toString();
                        if (titlebarFont.contains(QLatin1String("bold"), Qt::CaseInsensitive)) {
                            m_font->setBold(true);
                        }
                    }
                }
                watcher->deleteLater();
            });

    QDBusConnection::sessionBus().connect(
            QString(), QLatin1String("/org/freedesktop/portal/desktop"),
            QLatin1String("org.freedesktop.portal.Settings"), QLatin1String("SettingChanged"), this,
            SLOT(settingChanged(QString, QString, QDBusVariant)));

    updateColors(false);
    updateIcons();
}

void QAdwaitaDecorations::updateColors(bool useDarkColors)
{
    qCDebug(QAdwaitaDecorationsLog)
            << "Changing color scheme to " << (useDarkColors ? "dark" : "light");

    m_colors = { { Background, useDarkColors ? QColor(0x303030) : QColor(0xffffff) },
                 { BackgroundInactive, useDarkColors ? QColor(0x242424) : QColor(0xfafafa) },
                 { Foreground, useDarkColors ? QColor(0xffffff) : QColor(0x2e2e2e) },
                 { ForegroundInactive, useDarkColors ? QColor(0x919191) : QColor(0x949494) },
                 { Border, useDarkColors ? QColor(0x3b3b3b) : QColor(0xdbdbdb) },
                 { BorderInactive, useDarkColors ? QColor(0x303030) : QColor(0xdbdbdb) },
                 { ButtonBackground, useDarkColors ? QColor(0x444444) : QColor(0xebebeb) },
                 { ButtonBackgroundInactive, useDarkColors ? QColor(0x2e2e2e) : QColor(0xf0f0f0) },
                 { HoveredButtonBackground, useDarkColors ? QColor(0x4f4f4f) : QColor(0xe0e0e0) },
                 { PressedButtonBackground, useDarkColors ? QColor(0x6e6e6e) : QColor(0xc2c2c2) } };
    forceRepaint();
}

QString getIconSvg(const QString &iconName)
{
    const QStringList themeNames = { QIcon::themeName(), QIcon::fallbackThemeName(),
                                     QLatin1String("Adwaita") };
    qCDebug(QAdwaitaDecorationsLog) << "Icon themes: " << themeNames;

    for (const QString &themeName : themeNames) {
        for (const QString &path : QIcon::themeSearchPaths()) {
            if (path.startsWith(QLatin1Char(':')))
                continue;

            const QString fullPath = QString("%1/%2").arg(path).arg(themeName);
            QDirIterator dirIt(fullPath, QDirIterator::Subdirectories);
            while (dirIt.hasNext()) {
                const QString fileName = dirIt.next();
                const QFileInfo fileInfo(fileName);

                if (fileInfo.isDir())
                    continue;

                if (fileInfo.fileName() == iconName) {
                    qCDebug(QAdwaitaDecorationsLog)
                            << "Using " << iconName << " from " << themeName << " theme";
                    QFile readFile(fileInfo.filePath());
                    readFile.open(QFile::ReadOnly);
                    return readFile.readAll();
                }
            }
        }
    }

    qCWarning(QAdwaitaDecorationsLog) << "Failed to find an svg icon for " << iconName;

    return QString();
}

void QAdwaitaDecorations::updateIcons()
{
    for (auto mapIt = buttonMap.constBegin(); mapIt != buttonMap.constEnd(); mapIt++) {
        const QString fullName = mapIt.value() + QStringLiteral(".svg");
        m_icons[mapIt.key()] = getIconSvg(fullName);
    }

    forceRepaint();
}

void QAdwaitaDecorations::updateTitlebarLayout(const QString &layout)
{
    qCDebug(QAdwaitaDecorationsLog) << "Changing titlebar layout to " << layout;

    const QStringList layouts = layout.split(QLatin1Char(':'));
    if (layouts.count() != 2) {
        return;
    }

    // Remove previous configuration
    m_buttons.clear();

    const QString &leftLayout = layouts.at(0);
    const QString &rightLayout = layouts.at(1);
    m_placement = leftLayout.contains(QLatin1String("close")) ? Left : Right;

    int pos = 1;
    const QString &buttonLayout = m_placement == Right ? rightLayout : leftLayout;

    QStringList buttonList = buttonLayout.split(QLatin1Char(','));
    if (m_placement == Right) {
        std::reverse(buttonList.begin(), buttonList.end());
    }

    for (const QString &button : buttonList) {
        if (button == QLatin1String("close")) {
            m_buttons.insert(Close, pos);
        } else if (button == QLatin1String("maximize")) {
            m_buttons.insert(Maximize, pos);
        } else {
            m_buttons.insert(Minimize, pos);
        }
        pos++;
    }

    forceRepaint();
}

void QAdwaitaDecorations::settingChanged(const QString &group, const QString &key,
                                         const QDBusVariant &value)
{
    if (group == QLatin1String("org.gnome.desktop.wm.preferences")
        && key == QLatin1String("button-layout")) {
        const QString layout = value.variant().toString();
        updateTitlebarLayout(layout);
    } else if (group == QLatin1String("org.freedesktop.appearance")
               && key == QLatin1String("color-scheme")) {
        const uint colorScheme = value.variant().toUInt();
        updateColors(colorScheme == 1); // 1 == Prefer Dark
    }
}

QRectF QAdwaitaDecorations::buttonRect(Button button) const
{
    int xPos;
    int yPos;
    const int btnPos = m_buttons.value(button);

    if (m_placement == Right) {
        xPos = windowContentGeometry().width();
        xPos -= ceButtonWidth * btnPos;
        xPos -= ceButtonSpacing * btnPos;
#ifdef HAS_QT6_SUPPORT
        xPos -= margins(ShadowsOnly).right();
#endif
    } else {
        xPos = 0;
        xPos += ceButtonWidth * btnPos;
        xPos += ceButtonSpacing * btnPos;
#ifdef HAS_QT6_SUPPORT
        xPos += margins(ShadowsOnly).left();
#endif
        // We are painting from the left to the right so the real
        // position doesn't need to by moved by the size of the button.
        xPos -= ceButtonWidth;
    }

    yPos = margins().top();
    yPos += margins().bottom();
    yPos -= ceButtonWidth;
    yPos /= 2;

    return QRectF(xPos, yPos, ceButtonWidth, ceButtonWidth);
}

#ifdef HAS_QT6_SUPPORT
QMargins QAdwaitaDecorations::margins(MarginsType marginsType) const
{
    const bool onlyShadows = marginsType == ShadowsOnly;
    const bool shadowsExcluded = marginsType == ShadowsExcluded;

    if (waylandWindow()->windowStates() & Qt::WindowMaximized) {
        // Maximized windows don't have anything around, no shadows, border,
        // etc. Only report titlebar height in case we are not asking for shadow
        // margins.
        return QMargins(0, onlyShadows ? 0 : ceTitlebarHeight, 0, 0);
    }

    const QWaylandWindow::ToplevelWindowTilingStates tilingStates =
            waylandWindow()->toplevelWindowTilingStates();

    // Since all sides (left, right, bottom) are going to be same
    const int marginsBase =
            shadowsExcluded ? ceWindowBorderWidth : ceShadowsWidth + ceWindowBorderWidth;
    const int sideMargins = onlyShadows ? ceShadowsWidth : marginsBase;
    const int topMargins = onlyShadows ? ceShadowsWidth : ceTitlebarHeight + marginsBase;

    return QMargins(tilingStates & QWaylandWindow::WindowTiledLeft ? 0 : sideMargins,
                    tilingStates & QWaylandWindow::WindowTiledTop
                            ? onlyShadows ? 0 : ceTitlebarHeight
                            : topMargins,
                    tilingStates & QWaylandWindow::WindowTiledRight ? 0 : sideMargins,
                    tilingStates & QWaylandWindow::WindowTiledBottom ? 0 : sideMargins);
}
#else
QMargins QAdwaitaDecorations::margins() const
{
    if (window()->windowStates() & Qt::WindowMaximized) {
        // Maximized windows don't have anything around, no shadows, border,
        // etc. Only report titlebar height.
        return QMargins(0, ceTitlebarHeight, 0, 0);
    }

    return QMargins(ceWindowBorderWidth, ceWindowBorderWidth + ceTitlebarHeight,
                    ceWindowBorderWidth, ceWindowBorderWidth);
}
#endif

void QAdwaitaDecorations::paint(QPaintDevice *device)
{
#ifdef HAS_QT6_SUPPORT
    const Qt::WindowStates windowStates = waylandWindow()->windowStates();
    const bool active = windowStates & Qt::WindowActive;
    const bool tiled =
            waylandWindow()->toplevelWindowTilingStates() != QWaylandWindow::WindowNoState;
#else
    const Qt::WindowStates windowStates = window()->windowStates();
    const bool active = window()->handle()->isActive();
#endif
    const bool maximized = windowStates & Qt::WindowMaximized;

    const QRect surfaceRect = windowContentGeometry();

    const QColor borderColor = active ? m_colors[Border] : m_colors[BorderInactive];
    const QColor backgroundColor = active ? m_colors[Background] : m_colors[BackgroundInactive];
    const QColor foregroundColor = active ? m_colors[Foreground] : m_colors[ForegroundInactive];

    QPainter p(device);
    p.setRenderHint(QPainter::Antialiasing);

#ifdef HAS_QT6_SUPPORT
    // Shadows
    if (active && !(maximized || tiled)) {
        if (m_shadowPixmap.size() != surfaceRect.size()) {
            QPixmap source = QPixmap(surfaceRect.size());
            source.fill(Qt::transparent);
            {
                QRect topHalf = surfaceRect.translated(ceShadowsWidth, ceShadowsWidth);
                topHalf.setSize(QSize(surfaceRect.width() - (2 * ceShadowsWidth),
                                      surfaceRect.height() / 2));

                QRect bottomHalf = surfaceRect.translated(ceShadowsWidth, surfaceRect.height() / 2);
                bottomHalf.setSize(QSize(surfaceRect.width() - (2 * ceShadowsWidth),
                                         (surfaceRect.height() / 2) - ceShadowsWidth));

                QPainter tmpPainter(&source);
                tmpPainter.setBrush(borderColor);
                tmpPainter.drawRoundedRect(topHalf, ceCornerRadius, ceCornerRadius);
                tmpPainter.drawRect(bottomHalf);
                tmpPainter.end();
            }

            QImage backgroundImage(surfaceRect.size(), QImage::Format_ARGB32_Premultiplied);
            backgroundImage.fill(0);

            QPainter backgroundPainter(&backgroundImage);
            backgroundPainter.drawPixmap(QPointF(), source);
            backgroundPainter.end();

            QImage blurredImage(surfaceRect.size(), QImage::Format_ARGB32_Premultiplied);
            blurredImage.fill(0);
            {
                QPainter blurPainter(&blurredImage);
                qt_blurImage(&blurPainter, backgroundImage, 12, false, false);
                blurPainter.end();
            }
            backgroundImage = blurredImage;

            backgroundPainter.begin(&backgroundImage);
            backgroundPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            QRect rect = backgroundImage.rect().marginsRemoved(QMargins(8, 8, 8, 8));
            backgroundPainter.fillRect(rect, QColor(0, 0, 0, 160));
            backgroundPainter.end();

            m_shadowPixmap = QPixmap::fromImage(backgroundImage);
        }

        QRect clips[] = { QRect(0, 0, surfaceRect.width(), margins().top()),
                          QRect(0, margins().top(), margins().left(),
                                surfaceRect.height() - margins().top() - margins().bottom()),
                          QRect(0, surfaceRect.height() - margins().bottom(), surfaceRect.width(),
                                margins().bottom()),
                          QRect(surfaceRect.width() - margins().right(), margins().top(),
                                margins().right(),
                                surfaceRect.height() - margins().top() - margins().bottom()) };

        for (int i = 0; i < 4; ++i) {
            p.save();
            p.setClipRect(clips[i]);
            p.drawPixmap(QPoint(), m_shadowPixmap);
            p.restore();
        }
    }
#endif

    // Titlebar and window border
    {
        QPainterPath path;
        const int titleBarWidth = surfaceRect.width() - margins().left() - margins().right();
        const int borderRectHeight = surfaceRect.height() - margins().top() - margins().bottom();

#ifdef HAS_QT6_SUPPORT
        if (maximized || tiled)
#else
        if (maximized)
#endif
            path.addRect(margins().left(), margins().bottom(), titleBarWidth, margins().top());
        else
            path.addRoundedRect(margins().left(), margins().bottom(), titleBarWidth,
                                margins().top() + ceCornerRadius, ceCornerRadius, ceCornerRadius);

        p.save();
        p.setPen(borderColor);
        p.fillPath(path.simplified(), backgroundColor);
        p.drawPath(path);
        p.drawRect(margins().left(), margins().top(), titleBarWidth, borderRectHeight);
        p.restore();
    }

    // Titlebar separator
    {
        p.save();
        p.setPen(active ? m_colors[Border] : m_colors[BorderInactive]);
        p.drawLine(QLineF(margins().left(), margins().top() - ceTitlebarSeperatorWidth,
                          surfaceRect.width() - margins().right(),
                          margins().top() - ceTitlebarSeperatorWidth));
        p.restore();
    }

    // Window title
    {
        const QRect top = QRect(margins().left(), margins().bottom(), surfaceRect.width(),
                                margins().top() - margins().bottom());
#if QT_VERSION >= 0x060700
        const QString windowTitleText = waylandWindow()->windowTitle();
#else
        const QString windowTitleText = window()->title();
#endif
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
            p.setPen(foregroundColor);
            QSize size = m_windowTitle.size().toSize();
            int dx = (top.width() - size.width()) / 2;
            int dy = (top.height() - size.height()) / 2;
            p.setFont(*m_font);
            QPoint windowTitlePoint(top.topLeft().x() + dx, top.topLeft().y() + dy);
            p.drawStaticText(windowTitlePoint, m_windowTitle);
            p.restore();
        }
    }

    // Buttons
    {
        if (m_buttons.contains(Close))
            paintButton(Close, &p);

        if (m_buttons.contains(Maximize))
            paintButton(Maximize, &p);

        if (m_buttons.contains(Minimize))
            paintButton(Minimize, &p);
    }
}

static void renderFlatRoundedButtonFrame(QAdwaitaDecorations::Button button, QPainter *painter,
                                         const QRect &rect, const QColor &color)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawEllipse(rect);
    painter->restore();
}

static void renderButtonIcon(const QString &svgIcon, QPainter *painter, const QRect &rect,
                             const QColor &color)
{
    painter->save();
    painter->setRenderHints(QPainter::Antialiasing, true);

    QString icon = svgIcon;
    QRegularExpression regexp("fill=[\"']#[0-9A-F]{6}[\"']",
                              QRegularExpression::CaseInsensitiveOption);
    QRegularExpression regexpAlt("fill:#[0-9A-F]{6}", QRegularExpression::CaseInsensitiveOption);
    QRegularExpression regexpCurrentColor("fill=[\"']currentColor[\"']");
    icon.replace(regexp, QString("fill=\"%1\"").arg(color.name()));
    icon.replace(regexpAlt, QString("fill:%1").arg(color.name()));
    icon.replace(regexpCurrentColor, QString("fill=\"%1\"").arg(color.name()));
    QSvgRenderer svgRenderer(icon.toLocal8Bit());
    svgRenderer.render(painter, rect);

    painter->restore();
}

static void renderButtonIcon(QAdwaitaDecorations::ButtonIcon buttonIcon, QPainter *painter,
                             const QRect &rect)
{
    QString iconName = buttonMap[buttonIcon];

    painter->save();
    painter->setRenderHints(QPainter::Antialiasing, true);
    painter->drawPixmap(rect, QIcon::fromTheme(iconName).pixmap(ceButtonWidth, ceButtonWidth));

    painter->restore();
}

static QAdwaitaDecorations::ButtonIcon iconFromButtonAndState(QAdwaitaDecorations::Button button,
                                                              bool maximized)
{
    if (button == QAdwaitaDecorations::Close)
        return QAdwaitaDecorations::CloseIcon;
    else if (button == QAdwaitaDecorations::Minimize)
        return QAdwaitaDecorations::MinimizeIcon;
    else if (button == QAdwaitaDecorations::Maximize && maximized)
        return QAdwaitaDecorations::RestoreIcon;
    else
        return QAdwaitaDecorations::MaximizeIcon;
}

void QAdwaitaDecorations::paintButton(Button button, QPainter *painter)
{
#ifdef HAS_QT6_SUPPORT
    const Qt::WindowStates windowStates = waylandWindow()->windowStates();
    const bool active = windowStates & Qt::WindowActive;
#else
    const Qt::WindowStates windowStates = window()->windowStates();
    const bool active = window()->handle()->isActive();
#endif
    const bool maximized = windowStates & Qt::WindowMaximized;

    QColor activeBackgroundColor;
    if (m_clicking == button)
        activeBackgroundColor = m_colors[PressedButtonBackground];
    else if (m_hoveredButtons.testFlag(button))
        activeBackgroundColor = m_colors[HoveredButtonBackground];
    else
        activeBackgroundColor = m_colors[ButtonBackground];

    const QColor buttonBackgroundColor =
            active ? activeBackgroundColor : m_colors[ButtonBackgroundInactive];
    const QColor foregroundColor = active ? m_colors[Foreground] : m_colors[ForegroundInactive];

    const QRect btnRect = buttonRect(button).toRect();
    renderFlatRoundedButtonFrame(button, painter, btnRect, buttonBackgroundColor);

    QRect adjustedBtnRect = btnRect;
    adjustedBtnRect.setSize(QSize(16, 16));
    adjustedBtnRect.translate(4, 4);
    const QString svgIcon = m_icons[iconFromButtonAndState(button, maximized)];
    if (!svgIcon.isEmpty())
        renderButtonIcon(svgIcon, painter, adjustedBtnRect, foregroundColor);
    else // Fallback to use QIcon
        renderButtonIcon(iconFromButtonAndState(button, maximized), painter, adjustedBtnRect);
}

bool QAdwaitaDecorations::clickButton(Qt::MouseButtons b, Button btn)
{
    auto repaint = qScopeGuard([this] { forceRepaint(); });

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

bool QAdwaitaDecorations::doubleClickButton(Qt::MouseButtons b, const QPointF &local,
                                            const QDateTime &currentTime)
{
    if (isLeftClicked(b)) {
        const qint64 clickInterval = m_lastButtonClick.msecsTo(currentTime);
        m_lastButtonClick = currentTime;
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

bool QAdwaitaDecorations::handleMouse(QWaylandInputDevice *inputDevice, const QPointF &local,
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
    }

    // Reset clicking state in case a button press is released outside
    // the button area
    if (isLeftReleased(b)) {
        m_clicking = None;
        forceRepaint();
    }

    setMouseButtons(b);
    return false;
}

#if QT_VERSION >= 0x060000
bool QAdwaitaDecorations::handleTouch(QWaylandInputDevice *inputDevice, const QPointF &local,
                                      const QPointF &global, QEventPoint::State state,
                                      Qt::KeyboardModifiers mods)
#else
bool QAdwaitaDecorations::handleTouch(QWaylandInputDevice *inputDevice, const QPointF &local,
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
        } else if (m_buttons.contains(Maximize) && buttonRect(Maximize).contains(local)) {
            window()->setWindowStates(window()->windowStates() ^ Qt::WindowMaximized);
        } else if (m_buttons.contains(Minimize) && buttonRect(Minimize).contains(local)) {
            window()->setWindowState(Qt::WindowMinimized);
        } else if (local.y() <= margins().top()) {
            waylandWindow()->shellSurface()->move(inputDevice);
        } else {
            handled = false;
        }
    }

    return handled;
}

QRect QAdwaitaDecorations::windowContentGeometry() const
{
#ifdef HAS_QT6_SUPPORT
    return waylandWindow()->windowContentGeometry() + margins(ShadowsOnly);
#else
    return waylandWindow()->windowContentGeometry();
#endif
}

void QAdwaitaDecorations::forceRepaint()
{
    // Set dirty flag
    if (waylandWindow()->decoration()) {
        waylandWindow()->decoration()->update();
    }
    // Force re-paint
    // NOTE: not sure it's correct, but it's the only way to make it work
    if (waylandWindow()->backingStore()) {
        waylandWindow()->backingStore()->flush(window(), QRegion(), QPoint());
    }
}

void QAdwaitaDecorations::processMouseTop(QWaylandInputDevice *inputDevice, const QPointF &local,
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
            waylandWindow()->setMouseCursor(inputDevice, Qt::SizeVerCursor);
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
    } else if (m_buttons.contains(Maximize) && buttonRect(Maximize).contains(local)) {
        updateButtonHoverState(Maximize);
        if (clickButton(b, Maximize)) {
            window()->setWindowStates(window()->windowStates() ^ Qt::WindowMaximized);
            m_hoveredButtons.setFlag(Maximize, false);
        }
    } else if (m_buttons.contains(Minimize) && buttonRect(Minimize).contains(local)) {
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

void QAdwaitaDecorations::processMouseBottom(QWaylandInputDevice *inputDevice, const QPointF &local,
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
        waylandWindow()->setMouseCursor(inputDevice, Qt::SizeVerCursor);
#endif
        startResize(inputDevice, Qt::BottomEdge, b);
    }
}

void QAdwaitaDecorations::processMouseLeft(QWaylandInputDevice *inputDevice, const QPointF &local,
                                           Qt::MouseButtons b, Qt::KeyboardModifiers mods)
{
    Q_UNUSED(local)
    Q_UNUSED(mods)
#if QT_CONFIG(cursor)
    waylandWindow()->setMouseCursor(inputDevice, Qt::SizeHorCursor);
#endif
    startResize(inputDevice, Qt::LeftEdge, b);
}

void QAdwaitaDecorations::processMouseRight(QWaylandInputDevice *inputDevice, const QPointF &local,
                                            Qt::MouseButtons b, Qt::KeyboardModifiers mods)
{
    Q_UNUSED(local)
    Q_UNUSED(mods)
#if QT_CONFIG(cursor)
    waylandWindow()->setMouseCursor(inputDevice, Qt::SizeHorCursor);
#endif
    startResize(inputDevice, Qt::RightEdge, b);
}

bool QAdwaitaDecorations::updateButtonHoverState(Button hoveredButton)
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
