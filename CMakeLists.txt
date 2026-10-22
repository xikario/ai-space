cmake_minimum_required(VERSION 3.16)
project(AiSpace VERSION 0.1.1 LANGUAGES CXX)
include(GNUInstallDirs)
include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(AI_SPACE_QT_SDK_ROOT "${CMAKE_SOURCE_DIR}/.local-deps" CACHE PATH "Optional Qt development SDK")
find_package(Qt5 5.12 QUIET COMPONENTS Core Gui Widgets DBus Network)
if(NOT Qt5_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${AI_SPACE_QT_SDK_ROOT}/root/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/cmake")
    find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui Widgets DBus Network)
endif()
option(AI_SPACE_SANITIZERS "Enable ASan and UBSan for a separate Debug build" OFF)
if(AI_SPACE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
find_package(ZLIB REQUIRED)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
add_subdirectory(shared/async-work)
set(APP "${CMAKE_SOURCE_DIR}/apps/ai-space")
# Build-tree runs use a relative runtime snapshot, just like an installed prefix.
# No developer source-script fallback is compiled into the executable.
file(RELATIVE_PATH AI_SPACE_LIBEXEC_RELATIVE "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBEXECDIR}/ai-space")
file(RELATIVE_PATH AI_SPACE_SKILL_RELATIVE "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_DATADIR}/ai-space/skills/ai-space-index-query")
configure_file(${APP}/src/AppConfig.h.in ${CMAKE_BINARY_DIR}/generated/AppConfig.h @ONLY)
add_custom_target(ai-space-runtime ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${APP}/scripts ${CMAKE_BINARY_DIR}/libexec/ai-space
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${APP}/skills ${CMAKE_BINARY_DIR}/share/ai-space/skills
    COMMAND ${CMAKE_COMMAND} -E copy ${APP}/scripts/knowledge_library.py
        ${CMAKE_BINARY_DIR}/share/ai-space/skills/ai-space-index-query/scripts/knowledge_library.py)
add_library(ai-space-core STATIC
    ${APP}/src/Migration.cpp ${APP}/src/SmartSpaceWidget.cpp
    ${APP}/src/IndexSnapshotLoader.cpp ${APP}/src/PreviewPane.cpp
    ${APP}/src/SettingsPage.cpp ${APP}/src/KnowledgeOperations.cpp
    ${APP}/src/UiMessages.cpp ${APP}/src/UiMessages.h
    ${APP}/src/SpaceFormats.cpp ${APP}/src/SpaceFormats.h
    ${APP}/src/SpacePresentation.cpp ${APP}/src/SpacePresentation.h
    ${APP}/src/WindowCornerFallback.cpp ${APP}/src/WindowCornerFallback.h
    ${APP}/src/KnowledgeManagerDialog.cpp ${APP}/src/KnowledgeManagerDialog.h
    ${APP}/src/KnowledgeFailuresDialog.cpp ${APP}/src/KnowledgeFailuresDialog.h
    ${APP}/src/KnowledgeJobController.cpp ${APP}/src/KnowledgeJobController.h
    ${APP}/src/JsonWorkerRequest.cpp ${APP}/src/JsonWorkerRequest.h
    ${APP}/src/WindowTitleBarWidget.cpp ${APP}/src/WindowTitleBarWidget.h
    shared/desktop-launch/DesktopLaunchCompat.cpp)
target_include_directories(ai-space-core PUBLIC ${APP}/src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(ai-space-core PUBLIC Qt5::Widgets Qt5::DBus Qt5::Network Threads::Threads ZLIB::ZLIB async-work)
add_executable(ai-space ${APP}/src/main.cpp ${APP}/src/AiSpaceController.cpp ${APP}/resources.qrc)
target_link_libraries(ai-space PRIVATE ai-space-core)
add_dependencies(ai-space ai-space-runtime)
install(TARGETS ai-space RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS ${APP}/scripts/smart_space_indexer.py ${APP}/scripts/smart_space_knowledge.py
    ${APP}/scripts/smart_space_extract.py ${APP}/scripts/legacy_ppt.py ${APP}/scripts/knowledge_library.py
    ${APP}/scripts/knowledge_jobs.py ${APP}/scripts/ocr_budget.py ${APP}/scripts/provider_policy.py ${APP}/scripts/text_safety.py ${APP}/scripts/xml_safety.py ${APP}/scripts/ocr_environment.py ${APP}/scripts/gvariant_json.py ${APP}/scripts/failure_store.py ${APP}/scripts/ocr_cache.py ${APP}/scripts/rtf_text.py ${APP}/scripts/snapshot_io.py DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/ai-space)
install(DIRECTORY ${APP}/scripts/_vendor/ DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/ai-space/_vendor
    PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
install(FILES ${APP}/packaging/ai-space.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${APP}/assets/smart-space-icon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps RENAME ai-space.png)
configure_file(${APP}/packaging/install_desktop.cmake.in ${CMAKE_BINARY_DIR}/install_desktop.cmake @ONLY)
install(SCRIPT ${CMAKE_BINARY_DIR}/install_desktop.cmake)
install(FILES LICENSE THIRD_PARTY_NOTICES.md README.md CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ai-space)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ai-space/docs FILES_MATCHING PATTERN "*.md")
install(DIRECTORY ${APP}/skills/ DESTINATION ${CMAKE_INSTALL_DATADIR}/ai-space/skills
    PATTERN "__pycache__" EXCLUDE)
install(PROGRAMS ${APP}/scripts/knowledge_library.py DESTINATION ${CMAKE_INSTALL_DATADIR}/ai-space/skills/ai-space-index-query/scripts)
if(BUILD_TESTING)
    add_executable(ai-space-worker-protocol-test ${APP}/tests/worker_protocol_test.cpp
        ${APP}/src/JsonWorkerRequest.cpp ${APP}/src/JsonWorkerRequest.h
        ${APP}/src/KnowledgeJobController.cpp ${APP}/src/KnowledgeJobController.h)
    target_include_directories(ai-space-worker-protocol-test PRIVATE ${APP}/src)
    target_link_libraries(ai-space-worker-protocol-test PRIVATE Qt5::Core)
    add_test(NAME ai-space-worker-protocol COMMAND ai-space-worker-protocol-test)
    set_tests_properties(ai-space-worker-protocol PROPERTIES TIMEOUT 30)
    # Extra automation controls exist only in this non-installed test executable.
    add_executable(ai-space-test-driver ${APP}/src/main.cpp ${APP}/src/AiSpaceController.cpp ${APP}/resources.qrc)
    target_compile_definitions(ai-space-test-driver PRIVATE AI_SPACE_TEST_CONTROL)
    target_link_libraries(ai-space-test-driver PRIVATE ai-space-core)
    add_dependencies(ai-space-test-driver ai-space-runtime)
    add_test(NAME ai-space-portable-skill COMMAND ${Python3_EXECUTABLE} ${APP}/tests/portable_skill_test.py ${CMAKE_BINARY_DIR}/share/ai-space/skills/ai-space-index-query)
    set_tests_properties(ai-space-portable-skill PROPERTIES TIMEOUT 25)
    add_test(NAME ai-space-review-regressions COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${APP}/tests -p test_review_regressions.py -v)
    set_tests_properties(ai-space-review-regressions PROPERTIES TIMEOUT 120)
    add_test(NAME ai-space-data-boundaries COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${APP}/tests -p test_data_boundaries.py -v)
    add_test(NAME ai-space-provider-policy COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${APP}/tests -p test_provider_policy.py -v)
    add_test(NAME ai-space-publication COMMAND ${Python3_EXECUTABLE} ${APP}/tests/publication_test.py ${CMAKE_SOURCE_DIR})
    add_test(NAME ai-space-knowledge-library COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${APP}/tests -p test_knowledge_library.py -v)
    set_tests_properties(ai-space-knowledge-library PROPERTIES TIMEOUT 90)
    add_test(NAME ai-space-legacy-ppt COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${APP}/tests -p test_legacy_ppt.py -v)
    set_tests_properties(ai-space-legacy-ppt PROPERTIES TIMEOUT 45)
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
        add_test(NAME ai-space-public-api COMMAND ${XVFB_RUN} -a -s "-screen 0 1920x1080x24"
            ${DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE} ${APP}/tests/public_api_test.py $<TARGET_FILE:ai-space>)
        set_tests_properties(ai-space-public-api PROPERTIES TIMEOUT 45)
        foreach(SCALE 1 1.5 2)
            add_test(NAME ai-space-app-${SCALE} COMMAND ${XVFB_RUN} -a -s "-screen 0 2880x1800x24"
                ${DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE} ${APP}/tests/app_integration_test.py $<TARGET_FILE:ai-space-test-driver>)
            set_tests_properties(ai-space-app-${SCALE} PROPERTIES TIMEOUT 65
                ENVIRONMENT "QT_SCALE_FACTOR=${SCALE};QT_SCREEN_SCALE_FACTORS=1")
        endforeach()
        if(XDOTOOL)
            add_test(NAME ai-space-failure-actions-ui COMMAND ${XVFB_RUN} -a -s "-screen 0 1920x1080x24" ${DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE} ${APP}/tests/failure_actions_ui_test.py $<TARGET_FILE:ai-space-test-driver>)
            set_tests_properties(ai-space-failure-actions-ui PROPERTIES TIMEOUT 70 ENVIRONMENT "QT_SCALE_FACTOR=1;QT_SCREEN_SCALE_FACTORS=1")
            add_test(NAME ai-space-knowledge-ui COMMAND ${XVFB_RUN} -a -s "-screen 0 2880x1800x24" ${DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE} ${APP}/tests/knowledge_ui_test.py $<TARGET_FILE:ai-space-test-driver>)
            set_tests_properties(ai-space-knowledge-ui PROPERTIES TIMEOUT 70 ENVIRONMENT "QT_SCALE_FACTOR=1.5;QT_SCREEN_SCALE_FACTORS=1")
            add_test(NAME ai-space-knowledge-concurrent-ui COMMAND ${XVFB_RUN} -a -s "-screen 0 2880x1800x24" ${DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE} ${APP}/tests/knowledge_ui_test.py $<TARGET_FILE:ai-space-test-driver> --build-reader)
            set_tests_properties(ai-space-knowledge-concurrent-ui PROPERTIES TIMEOUT 70 ENVIRONMENT "QT_SCALE_FACTOR=1.5;QT_SCREEN_SCALE_FACTORS=1")
            foreach(SCALE 1 1.5 2)
                add_test(NAME ai-space-window-ui-${SCALE} COMMAND ${XVFB_RUN} -a -s "-screen 0 2880x1800x24"
                    ${DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE} ${APP}/tests/app_integration_test.py
                    $<TARGET_FILE:ai-space-test-driver> --window-ui)
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
                $<TARGET_FILE:ai-space-test-driver> --${MIGRATION_MODE}-migration)
            set_tests_properties(ai-space-migration-${MIGRATION_MODE} PROPERTIES TIMEOUT 65
                ENVIRONMENT "QT_SCALE_FACTOR=1;QT_SCREEN_SCALE_FACTORS=1")
        endforeach()
    endif()
endif()

# Portable deb output; installation and package creation do not run the app.
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "ai-space")
set(CPACK_PACKAGE_VENDOR "AI Space contributors")
set(CPACK_PACKAGE_CONTACT "AI Space contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Local document and offline knowledge search")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "AI Space contributors")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt5core5a (>= 5.12), libqt5gui5 (>= 5.12), libqt5widgets5 (>= 5.12), libqt5dbus5 (>= 5.12), libqt5network5 (>= 5.12), libx11-6, zlib1g, python3 (>= 3.8), util-linux")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "poppler-utils, tesseract-ocr, tesseract-ocr-chi-sim")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
