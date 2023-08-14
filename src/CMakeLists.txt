
set(qadwaitadecorations_SRCS
    qadwaitadecorationsplugin.cpp
    qadwaitadecorations.cpp
)

add_library(qadwaitadecorations MODULE ${qadwaitadecorations_SRCS})
target_link_libraries(qadwaitadecorations
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::GuiPrivate
    Qt${QT_VERSION_MAJOR}::WaylandClientPrivate
)

if (NOT USE_QT6)
    if (${Qt5XkbCommonSupport_FOUND})
    target_link_libraries(qadwaitadecorations
        Qt${QT_VERSION_MAJOR}::XkbCommonSupportPrivate
        Qt${QT_VERSION_MAJOR}::ThemeSupportPrivate
    )
    endif()
endif()

install(TARGETS qadwaitadecorations DESTINATION ${QT_PLUGINS_DIR}/wayland-decoration-client)

