cmake_minimum_required(VERSION 3.22)

project(OwlDocs VERSION 0.5.11 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)

option(DOCXSTUDIO_BUILD_APP "Build the Qt desktop application" ON)
option(DOCXSTUDIO_STRICT_WARNINGS "Treat compiler warnings as errors" OFF)
option(OWL_DOCS_REQUIRE_FIGURE_EDITOR_BUNDLE
       "Fail configuration unless the bundled Excalidraw runtime is built"
       OFF)
set(OWL_DOCS_ICON_GENERATOR_PYTHON "" CACHE FILEPATH
    "Optional Python interpreter with the pinned Pillow version for icon-generation tests")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(docxstudio_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)
    if(DOCXSTUDIO_STRICT_WARNINGS)
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>)
    endif()
endfunction()

add_library(docxstudio_core
    src/core/types.cpp
    src/core/formatting.cpp
    src/core/document.cpp
    src/core/document_session.cpp
)
target_include_directories(docxstudio_core PUBLIC include)
target_compile_features(docxstudio_core PUBLIC cxx_std_20)
docxstudio_warnings(docxstudio_core)

add_library(docxstudio_math
    src/math/ast.cpp
    src/math/latex_parser.cpp
)
target_include_directories(docxstudio_math PUBLIC include)
target_compile_features(docxstudio_math PUBLIC cxx_std_20)
docxstudio_warnings(docxstudio_math)

add_library(docxstudio_raster
    src/raster/validation.cpp
)
target_include_directories(docxstudio_raster PUBLIC include)
target_compile_features(docxstudio_raster PUBLIC cxx_std_20)
docxstudio_warnings(docxstudio_raster)
target_link_libraries(docxstudio_core PUBLIC docxstudio_raster)

pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
pkg_check_modules(PUGIXML REQUIRED IMPORTED_TARGET pugixml)
add_library(docxstudio_ooxml src/ooxml/docx_document.cpp)
target_include_directories(docxstudio_ooxml PUBLIC include)
target_link_libraries(docxstudio_ooxml PUBLIC
    docxstudio_math docxstudio_raster PkgConfig::LIBZIP PkgConfig::PUGIXML)
target_compile_features(docxstudio_ooxml PUBLIC cxx_std_20)
docxstudio_warnings(docxstudio_ooxml)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(LIBSECCOMP REQUIRED IMPORTED_TARGET libseccomp)
    add_library(docxstudio_worker
        src/worker/sandbox.cpp
        src/worker/protocol.cpp
        src/worker/client.cpp
    )
    target_include_directories(docxstudio_worker PUBLIC include)
    target_link_libraries(docxstudio_worker PUBLIC
        PkgConfig::LIBSECCOMP PkgConfig::LIBZIP PkgConfig::PUGIXML)
    target_compile_features(docxstudio_worker PUBLIC cxx_std_20)
    docxstudio_warnings(docxstudio_worker)

    add_executable(owl-docs-parser-worker src/worker/parser_worker_main.cpp)
    target_link_libraries(owl-docs-parser-worker PRIVATE docxstudio_worker)
    docxstudio_warnings(owl-docs-parser-worker)
    install(TARGETS owl-docs-parser-worker
            RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/owl-docs)
endif()

find_package(nlohmann_json CONFIG REQUIRED)

# Developer-only, transport-neutral DOCX preservation corpus runner. Corpus
# binaries live outside the source tree; see tests/corpus/README.md.
add_library(docxstudio_corpus STATIC
    tools/docx_corpus.cpp
)
target_include_directories(docxstudio_corpus PUBLIC tools)
target_link_libraries(docxstudio_corpus PUBLIC
    docxstudio_ooxml nlohmann_json::nlohmann_json PkgConfig::LIBZIP)
target_compile_features(docxstudio_corpus PUBLIC cxx_std_20)
docxstudio_warnings(docxstudio_corpus)

add_executable(owl-docs-corpus-runner
    tools/docx_corpus_runner.cpp
)
target_link_libraries(owl-docs-corpus-runner PRIVATE docxstudio_corpus)
docxstudio_warnings(owl-docs-corpus-runner)

add_library(docxstudio_codex
    src/codex/transport.cpp
    src/codex/client.cpp
    src/codex/editor_tools.cpp
    src/codex/redaction.cpp
)
target_include_directories(docxstudio_codex PUBLIC include)
target_link_libraries(docxstudio_codex PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(docxstudio_codex PUBLIC cxx_std_20)
docxstudio_warnings(docxstudio_codex)

if(DOCXSTUDIO_BUILD_APP)
    set(OWL_DOCS_FIGURE_EDITOR_BUNDLE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/excalidraw-figure-editor/dist/linux-unpacked")
    if(OWL_DOCS_REQUIRE_FIGURE_EDITOR_BUNDLE AND
       NOT EXISTS "${OWL_DOCS_FIGURE_EDITOR_BUNDLE}/owl-docs-figure-editor")
        message(FATAL_ERROR
            "Build third_party/excalidraw-figure-editor before configuring an official package")
    endif()
    find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)
    if(BUILD_TESTING)
        find_package(Qt6 6.2 REQUIRED COMPONENTS Test)
    endif()
    find_package(SQLite3 REQUIRED)
    pkg_check_modules(HUNSPELL REQUIRED IMPORTED_TARGET hunspell)

    add_library(docxstudio_app STATIC
        src/app/MainWindow.cpp
        src/app/RasterDecoder.cpp
        src/app/EditorPreferences.cpp
        src/app/FontFamilyPicker.cpp
        src/app/ListPropertiesDialog.cpp
        src/app/NavigationDock.cpp
        src/app/FileFingerprint.cpp
        src/app/CommandRegistry.cpp
        src/app/RibbonWidget.cpp
        src/app/ChatDock.cpp
        src/app/ChatStore.cpp
        src/app/RecoveryCodec.cpp
        src/app/SpellChecker.cpp
        src/app/DocumentCanvas.cpp
        src/app/QtDisplayListRenderer.cpp
        src/app/QtProcess.cpp
        src/app/CodexController.cpp
        src/app/EditorToolBridge.cpp
        src/app/ExcalidrawFigure.cpp
        src/app/OwlDocsIcon.cpp
        src/app/MathLayout.cpp
        include/docxstudio/app/MainWindow.h
        include/docxstudio/app/RasterDecoder.h
        include/docxstudio/app/EditorPreferences.h
        include/docxstudio/app/FontFamilyPicker.h
        include/docxstudio/app/ListPropertiesDialog.h
        include/docxstudio/app/NavigationDock.h
        include/docxstudio/app/FileFingerprint.h
        include/docxstudio/app/CommandRegistry.h
        include/docxstudio/app/RibbonWidget.h
        include/docxstudio/app/ChatDock.h
        include/docxstudio/app/DocumentCanvas.h
        include/docxstudio/app/CodexController.h
        include/docxstudio/app/QtProcess.h
        include/docxstudio/app/EditorToolBridge.h
        include/docxstudio/app/ExcalidrawFigure.h
        include/docxstudio/app/RecoveryCodec.h
        include/docxstudio/app/OwlDocsIcon.h
        include/docxstudio/app/MathLayout.h
    )
    set_target_properties(docxstudio_app PROPERTIES AUTOMOC ON)
    target_include_directories(docxstudio_app PUBLIC include)
    target_link_libraries(docxstudio_app PUBLIC
        docxstudio_core docxstudio_math docxstudio_raster docxstudio_ooxml docxstudio_codex docxstudio_worker
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::PrintSupport
        SQLite::SQLite3 PkgConfig::HUNSPELL
    )
    target_compile_definitions(docxstudio_app PUBLIC
        DOCXSTUDIO_VERSION="${PROJECT_VERSION}"
        DOCXSTUDIO_INSTALL_PARSER_WORKER_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/owl-docs/owl-docs-parser-worker"
        DOCXSTUDIO_INSTALL_FIGURE_EDITOR_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/owl-docs/figure-editor/owl-docs-figure-editor")
    if(NOT OWL_DOCS_REQUIRE_FIGURE_EDITOR_BUNDLE)
        target_compile_definitions(docxstudio_app PUBLIC
            DOCXSTUDIO_DEV_FIGURE_EDITOR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/excalidraw-figure-editor/dist/linux-unpacked/owl-docs-figure-editor")
    endif()
    add_dependencies(docxstudio_app owl-docs-parser-worker)
    docxstudio_warnings(docxstudio_app)

    add_executable(owl-docs
        src/app/main.cpp
        resources/owl-docs.qrc
    )
    set_target_properties(owl-docs PROPERTIES AUTORCC ON)
    target_link_libraries(owl-docs PRIVATE docxstudio_app)
    docxstudio_warnings(owl-docs)

    # Developer-side, opt-in fidelity probe. It intentionally uses MainWindow's
    # restricted DOCX import path and the same DocumentCanvas PDF renderer as
    # the desktop application, but is not part of release packages.
    add_executable(owl-docs-fidelity-probe EXCLUDE_FROM_ALL
        tools/owl_docs_fidelity_probe.cpp
    )
    target_link_libraries(owl-docs-fidelity-probe PRIVATE docxstudio_app)
    add_dependencies(owl-docs-fidelity-probe owl-docs-parser-worker)
    docxstudio_warnings(owl-docs-fidelity-probe)

    install(TARGETS owl-docs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(EXISTS "${OWL_DOCS_FIGURE_EDITOR_BUNDLE}/owl-docs-figure-editor")
        install(DIRECTORY
                "${OWL_DOCS_FIGURE_EDITOR_BUNDLE}/"
                DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/owl-docs/figure-editor
                USE_SOURCE_PERMISSIONS)
    endif()
    install(FILES packaging/debian/owl-docs.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/debian/owl-docs.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
    foreach(OWL_DOCS_ICON_SIZE 16 24 32 48 64 128 256 512)
        install(FILES
                packaging/icons/hicolor/${OWL_DOCS_ICON_SIZE}x${OWL_DOCS_ICON_SIZE}/apps/owl-docs.png
                DESTINATION
                ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${OWL_DOCS_ICON_SIZE}x${OWL_DOCS_ICON_SIZE}/apps)
    endforeach()
endif()

# Ship the exact status, compatibility, privacy, architecture, and corpus
# evidence documents that qualify this engineering release. Keep the package
# name stable rather than inheriting CMake's mixed-case project name.
install(FILES
        README.md
        docs/ARCHITECTURE.md
        docs/COMPATIBILITY.md
        docs/IMPLEMENTATION_STATUS.md
        docs/M0_CORPUS_EVIDENCE.md
        docs/PRIVACY.md
        docs/ROADMAP.md
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/owl-docs")
install(FILES
        third_party/excalidraw-figure-editor/THIRD_PARTY_NOTICES.md
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/owl-docs")

if(BUILD_TESTING)
    add_executable(docx_corpus_runner_tests
        tests/docx_corpus_runner_tests.cpp)
    target_link_libraries(docx_corpus_runner_tests PRIVATE docxstudio_corpus)
    docxstudio_warnings(docx_corpus_runner_tests)
    add_test(NAME docx_corpus_runner_tests COMMAND docx_corpus_runner_tests)

    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE docxstudio_core)
    docxstudio_warnings(core_tests)
    add_test(NAME core_tests COMMAND core_tests)

    add_executable(codex_tests tests/codex_tests.cpp)
    target_link_libraries(codex_tests PRIVATE docxstudio_codex)
    docxstudio_warnings(codex_tests)
    add_test(NAME codex_tests COMMAND codex_tests)

    add_executable(math_tests tests/math_tests.cpp)
    target_link_libraries(math_tests PRIVATE docxstudio_math)
    docxstudio_warnings(math_tests)
    add_test(NAME math_tests COMMAND math_tests)

    if(TARGET docxstudio_app)
        add_executable(math_layout_tests tests/math_layout_tests.cpp)
        target_link_libraries(math_layout_tests PRIVATE docxstudio_app)
        docxstudio_warnings(math_layout_tests)
        add_test(NAME math_layout_tests COMMAND math_layout_tests)
        set_tests_properties(math_layout_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-math-layout-tests")
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ooxml_tests.cpp")
        add_executable(ooxml_tests tests/ooxml_tests.cpp)
        target_link_libraries(ooxml_tests PRIVATE docxstudio_ooxml)
        docxstudio_warnings(ooxml_tests)
        add_test(NAME ooxml_tests COMMAND ooxml_tests)
    endif()

    add_executable(m0_feature_roundtrip_tests
        tests/m0_feature_roundtrip_tests.cpp)
    target_link_libraries(m0_feature_roundtrip_tests PRIVATE docxstudio_ooxml)
    docxstudio_warnings(m0_feature_roundtrip_tests)
    add_test(NAME m0_feature_roundtrip_tests
             COMMAND m0_feature_roundtrip_tests)

    if(TARGET docxstudio_worker)
        add_executable(worker_tests tests/worker_tests.cpp)
        target_link_libraries(worker_tests PRIVATE docxstudio_worker)
        target_compile_definitions(worker_tests PRIVATE
            DOCXSTUDIO_PARSER_WORKER_PATH="$<TARGET_FILE:owl-docs-parser-worker>")
        add_dependencies(worker_tests owl-docs-parser-worker)
        docxstudio_warnings(worker_tests)
        add_test(NAME worker_tests COMMAND worker_tests)
    endif()

    if(DOCXSTUDIO_BUILD_APP)
        add_executable(raster_decode_tests tests/raster_decode_tests.cpp)
        target_link_libraries(raster_decode_tests PRIVATE docxstudio_app)
        docxstudio_warnings(raster_decode_tests)
        add_test(NAME raster_decode_tests COMMAND raster_decode_tests)

        add_executable(app_smoke_tests tests/app_smoke_tests.cpp)
        target_link_libraries(app_smoke_tests PRIVATE docxstudio_app)
        docxstudio_warnings(app_smoke_tests)
        add_test(NAME app_smoke_tests COMMAND app_smoke_tests)
        set_tests_properties(app_smoke_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-tests")

        add_executable(editor_interaction_tests tests/editor_interaction_tests.cpp)
        target_link_libraries(editor_interaction_tests PRIVATE docxstudio_app)
        docxstudio_warnings(editor_interaction_tests)
        add_test(NAME editor_interaction_tests COMMAND editor_interaction_tests)
        set_tests_properties(editor_interaction_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-tests")

        add_executable(layout_conformance_tests
            tests/layout_conformance_tests.cpp)
        target_link_libraries(layout_conformance_tests PRIVATE docxstudio_app)
        docxstudio_warnings(layout_conformance_tests)
        add_test(NAME layout_conformance_tests COMMAND layout_conformance_tests)
        set_tests_properties(layout_conformance_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-layout-conformance-tests")

        add_executable(editor_preferences_tests tests/editor_preferences_tests.cpp)
        target_link_libraries(editor_preferences_tests PRIVATE docxstudio_app)
        docxstudio_warnings(editor_preferences_tests)
        add_test(NAME editor_preferences_tests COMMAND editor_preferences_tests)

        add_executable(main_window_preferences_tests
            tests/main_window_preferences_tests.cpp)
        target_link_libraries(main_window_preferences_tests PRIVATE docxstudio_app)
        docxstudio_warnings(main_window_preferences_tests)
        add_test(NAME main_window_preferences_tests
                 COMMAND main_window_preferences_tests)
        set_tests_properties(main_window_preferences_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-main-window-preferences-tests")

        add_executable(navigation_dock_tests
            tests/navigation_dock_tests.cpp)
        target_link_libraries(navigation_dock_tests PRIVATE docxstudio_app)
        docxstudio_warnings(navigation_dock_tests)
        add_test(NAME navigation_dock_tests COMMAND navigation_dock_tests)
        set_tests_properties(navigation_dock_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-navigation-dock-tests")

        add_executable(navigation_workflow_tests
            tests/navigation_workflow_tests.cpp)
        target_link_libraries(navigation_workflow_tests PRIVATE
            docxstudio_app Qt6::Test)
        docxstudio_warnings(navigation_workflow_tests)
        add_test(NAME navigation_workflow_tests COMMAND navigation_workflow_tests)
        set_tests_properties(navigation_workflow_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-navigation-workflow-tests")

        add_executable(file_workflow_tests
            tests/file_workflow_tests.cpp)
        target_link_libraries(file_workflow_tests PRIVATE docxstudio_app)
        docxstudio_warnings(file_workflow_tests)
        add_test(NAME file_workflow_tests COMMAND file_workflow_tests)
        set_tests_properties(file_workflow_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-file-workflow-tests")

        add_executable(list_properties_ui_tests
            tests/list_properties_ui_tests.cpp)
        target_link_libraries(list_properties_ui_tests PRIVATE docxstudio_app)
        docxstudio_warnings(list_properties_ui_tests)
        add_test(NAME list_properties_ui_tests COMMAND list_properties_ui_tests)
        set_tests_properties(list_properties_ui_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-list-properties-ui-tests")

        add_executable(table_ui_tests tests/table_ui_tests.cpp)
        target_link_libraries(table_ui_tests PRIVATE docxstudio_app)
        docxstudio_warnings(table_ui_tests)
        add_test(NAME table_ui_tests COMMAND table_ui_tests)
        set_tests_properties(table_ui_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-table-ui-tests")

        add_executable(ribbon_widget_tests tests/ribbon_widget_tests.cpp)
        target_link_libraries(ribbon_widget_tests PRIVATE docxstudio_app)
        docxstudio_warnings(ribbon_widget_tests)
        add_test(NAME ribbon_widget_tests COMMAND ribbon_widget_tests)
        set_tests_properties(ribbon_widget_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-ribbon-widget-tests")

        add_executable(equation_ui_tests tests/equation_ui_tests.cpp)
        target_link_libraries(equation_ui_tests PRIVATE docxstudio_app)
        docxstudio_warnings(equation_ui_tests)
        add_test(NAME equation_ui_tests COMMAND equation_ui_tests)
        set_tests_properties(equation_ui_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-equation-ui-tests")

        add_executable(image_ui_tests tests/image_ui_tests.cpp)
        target_link_libraries(image_ui_tests PRIVATE docxstudio_app)
        docxstudio_warnings(image_ui_tests)
        add_test(NAME image_ui_tests COMMAND image_ui_tests)
        set_tests_properties(image_ui_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-image-ui-tests")

        add_executable(excalidraw_figure_tests
            tests/excalidraw_figure_tests.cpp)
        target_link_libraries(excalidraw_figure_tests PRIVATE docxstudio_app)
        docxstudio_warnings(excalidraw_figure_tests)
        add_test(NAME excalidraw_figure_tests COMMAND excalidraw_figure_tests)
        set_tests_properties(excalidraw_figure_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-excalidraw-figure-tests")

        add_executable(picture_dialog_tests tests/picture_dialog_tests.cpp)
        target_link_libraries(picture_dialog_tests PRIVATE docxstudio_app)
        docxstudio_warnings(picture_dialog_tests)
        add_test(NAME picture_dialog_tests COMMAND picture_dialog_tests)
        set_tests_properties(picture_dialog_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-picture-dialog-tests")

        add_executable(print_tests tests/print_tests.cpp)
        target_link_libraries(print_tests PRIVATE docxstudio_app)
        docxstudio_warnings(print_tests)
        add_test(NAME print_tests COMMAND print_tests)
        set_tests_properties(print_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-print-tests")

        add_executable(combined_docx_roundtrip_tests tests/combined_docx_roundtrip_tests.cpp)
        target_link_libraries(combined_docx_roundtrip_tests PRIVATE docxstudio_app)
        docxstudio_warnings(combined_docx_roundtrip_tests)
        add_test(NAME combined_docx_roundtrip_tests COMMAND combined_docx_roundtrip_tests)
        set_tests_properties(combined_docx_roundtrip_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-combined-roundtrip-tests")

        add_executable(editor_tool_bridge_tests tests/editor_tool_bridge_tests.cpp)
        target_link_libraries(editor_tool_bridge_tests PRIVATE docxstudio_app)
        docxstudio_warnings(editor_tool_bridge_tests)
        add_test(NAME editor_tool_bridge_tests COMMAND editor_tool_bridge_tests)
        set_tests_properties(editor_tool_bridge_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-tests")

        add_executable(codex_controller_tests tests/codex_controller_tests.cpp)
        target_link_libraries(codex_controller_tests PRIVATE docxstudio_app)
        docxstudio_warnings(codex_controller_tests)
        add_test(NAME codex_controller_tests COMMAND codex_controller_tests)
        set_tests_properties(codex_controller_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-codex-controller-tests")

        add_executable(app_import_tests tests/app_import_tests.cpp)
        target_link_libraries(app_import_tests PRIVATE docxstudio_app)
        docxstudio_warnings(app_import_tests)
        add_test(NAME app_import_tests COMMAND app_import_tests)
        set_tests_properties(app_import_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-tests")

        add_executable(recovery_tests tests/recovery_tests.cpp)
        target_link_libraries(recovery_tests PRIVATE docxstudio_app)
        docxstudio_warnings(recovery_tests)
        add_test(NAME recovery_tests COMMAND recovery_tests)

        add_executable(file_fingerprint_tests tests/file_fingerprint_tests.cpp)
        target_link_libraries(file_fingerprint_tests PRIVATE docxstudio_app)
        docxstudio_warnings(file_fingerprint_tests)
        add_test(NAME file_fingerprint_tests COMMAND file_fingerprint_tests)

        add_executable(branding_tests
            tests/branding_tests.cpp
            resources/owl-docs.qrc
        )
        set_target_properties(branding_tests PROPERTIES AUTORCC ON)
        target_link_libraries(branding_tests PRIVATE docxstudio_app)
        target_compile_definitions(branding_tests PRIVATE
            OWL_DOCS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        docxstudio_warnings(branding_tests)
        add_test(NAME branding_tests COMMAND branding_tests)
        set_tests_properties(branding_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-branding-tests")
    endif()
endif()

set(OWL_DOCS_CORPUS_MANIFEST "" CACHE FILEPATH
    "Optional external owl-docs-corpus-v1 manifest for the 100+ DOCX gate")
if(BUILD_TESTING AND OWL_DOCS_CORPUS_MANIFEST)
    add_test(NAME public_docx_corpus
        COMMAND owl-docs-corpus-runner
            --manifest "${OWL_DOCS_CORPUS_MANIFEST}"
            --ephemeral-output
            --minimum-fixtures 100)
    set_tests_properties(public_docx_corpus PROPERTIES TIMEOUT 900)
endif()

if(BUILD_TESTING AND OWL_DOCS_ICON_GENERATOR_PYTHON)
    if(NOT EXISTS "${OWL_DOCS_ICON_GENERATOR_PYTHON}")
        message(FATAL_ERROR
            "OWL_DOCS_ICON_GENERATOR_PYTHON does not name an existing interpreter")
    endif()
    add_test(NAME icon_generation_tests
        COMMAND "${OWL_DOCS_ICON_GENERATOR_PYTHON}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_app_icons.py"
                --check)
    set_tests_properties(icon_generation_tests PROPERTIES
        LABELS "branding;maintenance")
endif()

# When a Codex runtime and Python are present on the build host, verify the
# exact experimental fields used by Owl Docs against app-server's generated
# protocol schema. Builds remain possible on fully offline hosts without Codex.
if(BUILD_TESTING)
    find_program(OWL_DOCS_CODEX_SCHEMA_EXECUTABLE NAMES codex
                 HINTS /usr/lib/chatgpt/resources)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(OWL_DOCS_CODEX_SCHEMA_EXECUTABLE AND Python3_Interpreter_FOUND)
        add_test(NAME codex_app_server_schema_compatibility
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_codex_app_server_schema.py"
                    --codex "${OWL_DOCS_CODEX_SCHEMA_EXECUTABLE}")
        set_tests_properties(codex_app_server_schema_compatibility PROPERTIES
            LABELS "codex;compatibility" TIMEOUT 45)
    endif()
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "owl-docs")
set(CPACK_PACKAGE_VENDOR "Owl Docs")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Offline word processor with optional Codex assistance")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
    "Offline DOCX word processor with optional Codex assistance.\n Owl Docs provides local document editing, PDF export, and an optional\n user-enabled Codex chat experience.")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Owl Docs")
set(CPACK_DEBIAN_PACKAGE_REPLACES "docx-studio")
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "docx-studio")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "editors")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "bubblewrap, hunspell-en-us, fonts-crosextra-carlito, qt6-qpa-plugins")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
set(CPACK_STRIP_FILES ON)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)
include(CPack)
