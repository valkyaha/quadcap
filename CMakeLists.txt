cmake_minimum_required(VERSION 3.22)

project(quadcap VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

include(CTest)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Test)
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
    IMPORT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/qml-stubs"
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

# Packages must not inherit the builder's umask, and this source tree lives on a mount that reports
# every file as 777, so directory modes are pinned rather than copied.
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

install(TARGETS quadcap quadcapd RUNTIME DESTINATION bin)
install(FILES packaging/quadcap.desktop DESTINATION share/applications)
install(FILES packaging/quadcap.svg DESTINATION share/icons/hicolor/scalable/apps)
install(PROGRAMS packaging/install.sh packaging/uninstall.sh DESTINATION share/quadcap)
install(FILES INSTALL.md packaging/quadcap-edid.service DESTINATION share/quadcap)
install(DIRECTORY packaging/edid DESTINATION share/quadcap)

# Shared by the changelog trailer below and by the package metadata further down, which is set
# after these install rules and so cannot be read from here.
set(QUADCAP_MAINTAINER "quadcap maintainers <darkvalhalla@proton.me>")

# Debian expects a copyright file and a changelog under /usr/share/doc, and lintian treats their
# absence as an error. The changelog is generated so it cannot drift from the project version.
install(FILES packaging/debian/copyright DESTINATION share/doc/${PROJECT_NAME})
# A Debian changelog trailer must carry an RFC822 date in English. string(TIMESTAMP "%a %b") is
# localised, so on a machine with a Spanish locale it writes "vie, 19 sep" and dpkg rejects the
# entry. Both names are therefore derived from numbers instead.
string(TIMESTAMP QUADCAP_EPOCH "%s" UTC)
string(TIMESTAMP QUADCAP_DAY "%d" UTC)
string(TIMESTAMP QUADCAP_MONTH "%m" UTC)
string(TIMESTAMP QUADCAP_YEAR "%Y" UTC)
string(TIMESTAMP QUADCAP_CLOCK "%H:%M:%S" UTC)
# 1970-01-01 was a Thursday, so the epoch day count is offset by four to land Sunday on zero.
math(EXPR QUADCAP_WEEKDAY "((${QUADCAP_EPOCH} / 86400) + 4) % 7")
set(QUADCAP_WEEKDAYS Sun Mon Tue Wed Thu Fri Sat)
set(QUADCAP_MONTHS Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec)
list(GET QUADCAP_WEEKDAYS ${QUADCAP_WEEKDAY} QUADCAP_WEEKDAY_NAME)
math(EXPR QUADCAP_MONTH_INDEX "${QUADCAP_MONTH} - 1")
list(GET QUADCAP_MONTHS ${QUADCAP_MONTH_INDEX} QUADCAP_MONTH_NAME)
set(QUADCAP_CHANGELOG_DATE
    "${QUADCAP_WEEKDAY_NAME}, ${QUADCAP_DAY} ${QUADCAP_MONTH_NAME} ${QUADCAP_YEAR} ${QUADCAP_CLOCK} +0000")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/changelog"
     "quadcap (${PROJECT_VERSION}) unstable; urgency=medium\n\n"
     "  * Release ${PROJECT_VERSION}. See the project release page for the changes.\n\n"
     " -- ${QUADCAP_MAINTAINER}  ${QUADCAP_CHANGELOG_DATE}\n")
# -n keeps the timestamp out of the gzip header, so the same source produces the same bytes.
execute_process(
    COMMAND gzip -9n -c "${CMAKE_CURRENT_BINARY_DIR}/changelog"
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz"
    RESULT_VARIABLE QUADCAP_GZIP_RESULT)
if(NOT QUADCAP_GZIP_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not compress the Debian changelog")
endif()
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz" DESTINATION share/doc/${PROJECT_NAME})

foreach(page quadcap quadcapd)
    execute_process(
        COMMAND gzip -9n -c "${CMAKE_CURRENT_SOURCE_DIR}/packaging/man/${page}.1"
        OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/${page}.1.gz"
        RESULT_VARIABLE QUADCAP_MAN_RESULT)
    if(NOT QUADCAP_MAN_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not compress the ${page} manual page")
    endif()
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${page}.1.gz" DESTINATION share/man/man1)
endforeach()

# Packaging. The .deb carries the application only: the sc0710 driver is an out-of-tree module that
# needs DKMS and, under Secure Boot, an interactive MOK enrolment, neither of which belongs in a
# package postinst. packaging/install.sh handles that half and ships alongside.
set(CPACK_PACKAGE_NAME quadcap)
set(CPACK_PACKAGE_VENDOR quadcap)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Capture and record a games console through an Elgato 4K60 Pro MK.2")
set(CPACK_PACKAGE_DESCRIPTION
    "quadcap records gameplay from a console attached to an Elgato 4K60 Pro
MK.2 while passing the picture through to a monitor untouched. It previews
on the GPU at capture rate, keeps a rolling flashback buffer so the last few
minutes can be saved after the fact, and records the game and the microphone
as separate tracks beside a mixed one.
.
The card needs the out-of-tree sc0710 kernel module, which this package does
not install. Run /usr/share/quadcap/install.sh to set it up through DKMS,
including the Secure Boot signing that lets it load.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/valkyaha/quadcap")
set(CPACK_PACKAGE_CONTACT "${QUADCAP_MAINTAINER}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "quadcap_${PROJECT_VERSION}_amd64")
set(CPACK_STRIP_FILES ON)

set(CPACK_GENERATOR DEB)
set(CPACK_DEBIAN_PACKAGE_SECTION video)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
# dpkg-shlibdeps resolves everything quadcap links against. QML modules and GStreamer plugins are
# loaded by name at runtime, so nothing links them and they have to be named by hand.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "qml6-module-qtqml, qml6-module-qtqml-workerscript, qml6-module-qtquick, \
qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtquick-templates, \
qml6-module-qtquick-window, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, \
gstreamer1.0-plugins-bad, gstreamer1.0-alsa, gstreamer1.0-qt6")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "dkms, gstreamer1.0-pipewire, gstreamer1.0-plugins-ugly")
include(CPack)
