cmake_minimum_required(VERSION 3.16)
project(AiSpace VERSION 0.1.1 LANGUAGES CXX)
include(GNUInstallDirs)
include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(AI_SPACE_QT_SDK_ROOT "${CMAKE_SOURCE_DIR}/.local-deps" CACHE PATH "Optional Qt development SDK")
find_package(Qt5 QUIET COMPONENTS Core Gui Widgets DBus Network)
if(NOT Qt5_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${AI_SPACE_QT_SDK_ROOT}/root/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/cmake")
    find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets DBus Network)
endif()
find_package(ZLIB REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
add_subdirectory(shared/async-work)
set(APP "${CMAKE_SOURCE_DIR}/apps/ai-space")
add_executable(ai-space
    ${APP}/src/main.cpp ${APP}/src/AiSpaceController.cpp ${APP}/src/Migration.cpp
    ${APP}/src/SmartSpaceWidget.cpp
    ${APP}/src/WindowTitleBarWidget.cpp ${APP}/src/WindowTitleBarWidget.h
    shared/desktop-launch/DesktopLaunchCompat.cpp ${APP}/resources.qrc)
target_include_directories(ai-space PRIVATE ${APP}/src)
target_compile_definitions(ai-space PRIVATE
    AI_SPACE_INDEXER_PATH="${APP}/scripts/smart_space_indexer.py"
    AI_SPACE_KNOWLEDGE_WORKER_PATH="${APP}/scripts/smart_space_knowledge.py"
    AI_SPACE_INDEX_SKILL_PATH="${APP}/skills/ai-space-index-query")
target_link_libraries(ai-space PRIVATE Qt5::Widgets Qt5::DBus Qt5::Network Threads::Threads ZLIB::ZLIB)
install(TARGETS ai-space RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS ${APP}/scripts/smart_space_indexer.py ${APP}/scripts/smart_space_knowledge.py
    ${APP}/scripts/smart_space_extract.py DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/ai-space)
install(DIRECTORY ${APP}/scripts/_vendor/ DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/ai-space/_vendor
    PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
install(FILES ${APP}/packaging/ai-space.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${APP}/assets/smart-space-icon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps RENAME ai-space.png)
configure_file(${APP}/packaging/install_desktop.cmake.in ${CMAKE_BINARY_DIR}/install_desktop.cmake @ONLY)
install(SCRIPT ${CMAKE_BINARY_DIR}/install_desktop.cmake)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ai-space)
install(DIRECTORY ${APP}/skills/ DESTINATION ${CMAKE_INSTALL_DATADIR}/ai-space/skills
    PATTERN "__pycache__" EXCLUDE)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(KIND indexer knowledge extract)
        add_test(NAME ai-space-${KIND} COMMAND ${Python3_EXECUTABLE} -m unittest -v tests/test_smart_space_${KIND}.py)
        set_tests_properties(ai-space-${KIND} PROPERTIES WORKING_DIRECTORY ${APP} TIMEOUT 180)
    endforeach()
    # Includes two real multi-page OCR passes, deliberately running at low
    # process priority. Keep UI suites out of their CPU budget on ARM machines.
    set_tests_properties(ai-space-indexer PROPERTIES TIMEOUT 420 RUN_SERIAL TRUE)
    find_program(XVFB_RUN xvfb-run)
    find_program(DBUS_RUN_SESSION dbus-run-session)
    find_program(XDOTOOL xdotool)
    if(XVFB_RUN AND DBUS_RUN_SESSION)
        foreach(SCALE 1 1.5 2)
            add_test(NAME ai-space-app-${SCALE} COMMAND ${XVFB_RUN} -a -s "-screen 0 2880x1800x24"
                ${DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE} ${APP}/tests/app_integration_test.py $<TARGET_FILE:ai-space>)
            set_tests_properties(ai-space-app-${SCALE} PROPERTIES TIMEOUT 65
                ENVIRONMENT "QT_SCALE_FACTOR=${SCALE};QT_SCREEN_SCALE_FACTORS=1")
        endforeach()
        if(XDOTOOL)
            foreach(SCALE 1 1.5 2)
                add_test(NAME ai-space-window-ui-${SCALE} COMMAND ${XVFB_RUN} -a -s "-screen 0 2880x1800x24"
                    ${DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE} ${APP}/tests/app_integration_test.py
                    $<TARGET_FILE:ai-space> --window-ui)
                set_tests_properties(ai-space-window-ui-${SCALE} PROPERTIES TIMEOUT 65
                    ENVIRONMENT "QT_SCALE_FACTOR=${SCALE};QT_SCREEN_SCALE_FACTORS=1")
            endforeach()
        endif()
        add_test(NAME ai-space-installed COMMAND ${XVFB_RUN} -a -s "-screen 0 2880x1800x24"
            ${DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE} ${APP}/tests/installed_package_test.py ${CMAKE_BINARY_DIR})
        set_tests_properties(ai-space-installed PROPERTIES TIMEOUT 90
            ENVIRONMENT "QT_SCALE_FACTOR=1;QT_SCREEN_SCALE_FACTORS=1")
        foreach(MIGRATION_MODE locked cancel)
            add_test(NAME ai-space-migration-${MIGRATION_MODE} COMMAND ${XVFB_RUN} -a -s "-screen 0 2880x1800x24"
                ${DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE} ${APP}/tests/app_integration_test.py
                $<TARGET_FILE:ai-space> --${MIGRATION_MODE}-migration)
            set_tests_properties(ai-space-migration-${MIGRATION_MODE} PROPERTIES TIMEOUT 65
                ENVIRONMENT "QT_SCALE_FACTOR=1;QT_SCREEN_SCALE_FACTORS=1")
        endforeach()
    endif()
endif()
