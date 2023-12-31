include(ECMMarkAsTest)

# find_package(Qt6Core ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENT Private _check_private)
include_directories(SYSTEM ${Qt6Core_PRIVATE_INCLUDE_DIRS})
set(testServer_SRCS
    waylandservertest.cpp
    fakeoutput.cpp
    )
add_executable(testServer ${testServer_SRCS})
target_link_libraries(testServer kwin Qt::CorePrivate)
ecm_mark_as_test(testServer)

find_package(Qt6Widgets ${QT_MIN_VERSION} CONFIG QUIET)
if (TARGET Qt::Widgets)
    set(testRenderingServer_SRCS
        renderingservertest.cpp
        fakeoutput.cpp
        )
    add_executable(testRenderingServer ${testRenderingServer_SRCS})
    target_link_libraries(testRenderingServer kwin Qt::Core Qt::Widgets)
    ecm_mark_as_test(testRenderingServer)
endif()

add_executable(copyClient copyclient.cpp)
target_link_libraries(copyClient KF6::WaylandClient)
ecm_mark_as_test(copyClient)

add_executable(pasteClient pasteclient.cpp)
target_link_libraries(pasteClient Qt::Core KF6::WaylandClient)
ecm_mark_as_test(pasteClient)

add_executable(touchClientTest touchclienttest.cpp)
target_link_libraries(touchClientTest KF6::WaylandClient)

add_executable(panelTest paneltest.cpp)
target_link_libraries(panelTest KF6::WaylandClient)
ecm_mark_as_test(panelTest)

add_executable(subsurface-test subsurfacetest.cpp)
target_link_libraries(subsurface-test Qt::Core Qt::Gui KF6::WaylandClient)
ecm_mark_as_test(subsurface-test)

add_executable(shadowTest shadowtest.cpp)
target_link_libraries(shadowTest KF6::WaylandClient)
ecm_mark_as_test(shadowTest)


if (TARGET Qt::Widgets)
    add_executable(dpmsTest dpmstest.cpp)
    target_link_libraries(dpmsTest KF6::WaylandClient Qt::Widgets)
    ecm_mark_as_test(dpmsTest)
endif()

add_executable(plasmasurface-test plasmasurfacetest.cpp)
target_link_libraries(plasmasurface-test Qt::Gui KF6::WaylandClient)
ecm_mark_as_test(plasmasurface-test)

add_executable(xdgforeign-test xdgforeigntest.cpp)
target_link_libraries(xdgforeign-test Qt::Gui KF6::WaylandClient)
ecm_mark_as_test(xdgforeign-test)

add_executable(xdg-test xdgtest.cpp)
target_link_libraries(xdg-test Qt::Gui KF6::WaylandClient)
ecm_mark_as_test(xdg-test)

