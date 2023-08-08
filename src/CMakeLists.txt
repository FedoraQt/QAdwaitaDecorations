
set(qadwaitadecoration_SRCS
    qadwaitadecorationplugin.cpp
    qadwaitadecoration.cpp
)

add_library(qadwaitadecoration MODULE ${qadwaitadecoration_SRCS})
target_link_libraries(qadwaitadecoration
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::WaylandClientPrivate
)

if (NOT USE_QT6)
    if (${Qt5XkbCommonSupport_FOUND})
    target_link_libraries(qadwaitadecoration
        Qt${QT_VERSION_MAJOR}::XkbCommonSupportPrivate
    )
    endif()
endif()

install(TARGETS qadwaitadecoration DESTINATION ${QT_PLUGINS_DIR}/wayland-decoration-client)

