cmake_minimum_required(VERSION 3.22)

project(quadcap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

include(CTest)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0>=1.22
    gstreamer-video-1.0>=1.22
)

add_library(quadcap_device
    src/device/CaptureDevice.cpp
    src/device/CaptureDevice.h
    src/device/SystemCheck.cpp
    src/device/SystemCheck.h
    src/device/DeviceDiscovery.cpp
    src/device/DeviceDiscovery.h
    src/device/DeviceTypes.cpp
    src/device/DeviceTypes.h
)
target_include_directories(quadcap_device PUBLIC src)
target_link_libraries(quadcap_device PUBLIC Qt6::Core)
target_compile_options(quadcap_device PRIVATE -Wall -Wextra -Wpedantic)

add_library(quadcap_pipeline
    src/pipeline/CapturePipeline.cpp
    src/pipeline/CapturePipeline.h
)
target_include_directories(quadcap_pipeline PUBLIC src)
target_link_libraries(quadcap_pipeline PUBLIC Qt6::Core PkgConfig::GSTREAMER)
target_compile_options(quadcap_pipeline PRIVATE -Wall -Wextra -Wpedantic)

add_library(quadcap_flashback
    src/flashback/FlashbackRing.cpp
    src/flashback/FlashbackRing.h
)
target_include_directories(quadcap_flashback PUBLIC src)
target_link_libraries(quadcap_flashback PUBLIC Qt6::Core PkgConfig::GSTREAMER)
target_compile_options(quadcap_flashback PRIVATE -Wall -Wextra -Wpedantic)

add_executable(quadcapd src/control/main.cpp)
target_link_libraries(quadcapd PRIVATE quadcap_device quadcap_pipeline Qt6::Core)

add_library(quadcap_singleinstance
    src/ui/SingleInstance.cpp
    src/ui/SingleInstance.h
)
target_include_directories(quadcap_singleinstance PUBLIC src)
target_link_libraries(quadcap_singleinstance PUBLIC Qt6::Core Qt6::Network)
target_compile_options(quadcap_singleinstance PRIVATE -Wall -Wextra -Wpedantic)

qt_add_executable(quadcap
    src/ui/main.cpp
    src/ui/AppController.cpp
    src/ui/AppController.h
)
qt_add_qml_module(quadcap
    URI Quadcap
    VERSION 1.0
    QML_FILES src/ui/Main.qml
    # Without this the module lands at a resource path the engine does not search, so the binary
    # only works when a Quadcap/ directory happens to sit beside it — that is, from the build tree.
    # Installed to /usr/local/bin it failed with 'Module "Quadcap" contains no type named "Main"'.
    # /qt/qml is on the default QML import path, so the module is found wherever the binary runs.
    RESOURCE_PREFIX "/qt/qml"
)
target_link_libraries(quadcap PRIVATE
    quadcap_device
    quadcap_pipeline
    quadcap_flashback
    quadcap_singleinstance
    Qt6::Core
    Qt6::Network
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)
target_compile_options(quadcap PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(device_tests tests/unit/DeviceTests.cpp)
    target_link_libraries(device_tests PRIVATE quadcap_device Qt6::Test)
    add_test(NAME device_tests COMMAND device_tests)

    add_executable(pipeline_tests tests/integration/PipelineTests.cpp)
    target_link_libraries(pipeline_tests PRIVATE quadcap_pipeline Qt6::Test)
    add_test(NAME pipeline_tests COMMAND pipeline_tests)
    set_tests_properties(pipeline_tests PROPERTIES TIMEOUT 60)

    add_executable(single_instance_tests tests/unit/SingleInstanceTests.cpp)
    target_link_libraries(single_instance_tests PRIVATE quadcap_singleinstance Qt6::Test Qt6::Gui)
    add_test(NAME single_instance_tests COMMAND single_instance_tests)
    set_tests_properties(single_instance_tests PROPERTIES TIMEOUT 30)

    add_executable(flashback_tests tests/integration/FlashbackTests.cpp)
    target_link_libraries(flashback_tests PRIVATE quadcap_flashback quadcap_pipeline Qt6::Test)
    add_test(NAME flashback_tests COMMAND flashback_tests)
    set_tests_properties(flashback_tests PROPERTIES TIMEOUT 90)
endif()

install(TARGETS quadcap quadcapd RUNTIME DESTINATION bin)
install(FILES packaging/quadcap.desktop DESTINATION share/applications)
install(FILES packaging/quadcap.svg DESTINATION share/icons/hicolor/scalable/apps)
install(PROGRAMS packaging/install.sh packaging/uninstall.sh DESTINATION share/quadcap)
install(FILES INSTALL.md packaging/quadcap-edid.service DESTINATION share/quadcap)
install(DIRECTORY packaging/edid DESTINATION share/quadcap)
