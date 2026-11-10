cmake_minimum_required(VERSION 3.22)

project(OwlDocs VERSION 0.6.5 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)

option(DOCXSTUDIO_BUILD_APP "Build the Qt desktop application" ON)
option(DOCXSTUDIO_STRICT_WARNINGS "Treat compiler warnings as errors" OFF)
option(OWL_DOCS_REQUIRE_FIGURE_EDITOR_BUNDLE
       "Fail configuration unless the bundled Excalidraw runtime is built"
       OFF)
set(OWL_DOCS_ICON_GENERATOR_PYTHON "" CACHE FILEPATH
    "Optional Python interpreter with a reviewed Pillow version for icon-generation tests")

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
    src/core/academic.cpp
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
    src/codex/authoring_tools.cpp
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
        src/app/DocumentCanvasTableEditing.cpp
        src/app/DocumentCanvasListPlanning.cpp
        src/app/QtDisplayListRenderer.cpp
        src/app/QtProcess.cpp
        src/app/CodexController.cpp
        src/app/EditorToolBridge.cpp
        src/app/EditorSemanticOperations.cpp
        src/app/ExcalidrawFigure.cpp
        src/app/ExcalidrawScene.cpp
        src/app/OwlDocsIcon.cpp
        src/app/MathLayout.cpp
        src/app/EquationEditorDialog.cpp
        src/app/ColorPaletteWidget.cpp
        src/app/TableInsertDialog.cpp
        include/docxstudio/app/TableInsertDialog.h
        include/docxstudio/app/ColorPaletteWidget.h
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
        include/docxstudio/app/EditorSemanticOperations.h
        include/docxstudio/app/ExcalidrawFigure.h
        include/docxstudio/app/ExcalidrawScene.h
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
        docs/RELEASE_NOTES_0.6.0.md
        docs/RELEASE_NOTES_0.6.1.md
        docs/RELEASE_NOTES_0.6.2.md
        docs/RELEASE_NOTES_0.6.3.md
        docs/RELEASE_NOTES_0.6.4.md
        docs/RELEASE_NOTES_0.6.5.md
        docs/CODEX_AUTHORING.md
        docs/TDD_CODEX_AUTHORING_0.6.5.md
        docs/TDD_CODEX_CONTROLLER_0.6.5.md
        docs/TDD_CODEX_FIGURES_0.6.5.md
        docs/RELEASE_VALIDATION_0.6.0.md
        docs/RELEASE_VALIDATION_0.6.1.md
        docs/RELEASE_VALIDATION_0.6.2.md
        docs/RELEASE_VALIDATION_0.6.3.md
        docs/RELEASE_VALIDATION_0.6.4.md
        docs/RELEASE_VALIDATION_0.6.5.md
        docs/ROADMAP.md
        docs/TDD_ACADEMIC_EVIDENCE.md
        docs/TDD_FEEDBACK_0.6.1.md
        docs/TDD_FEEDBACK_0.6.2.md
        docs/TDD_COLORS_0.6.2.md
        docs/TDD_MARGINS_0.6.2.md
        docs/TDD_TABLE_IMAGES_0.6.2.md
        docs/TDD_TABLE_PICKER_0.6.2.md
        docs/TDD_TABLE_MODEL_0.6.3.md
        docs/TDD_TABLE_LAYOUT_0.6.3.md
        docs/TDD_TABLE_EDITING_UI_0.6.3.md
        docs/TDD_TABLE_RIBBON.md
        docs/TDD_TABLE_RIBBON_0.6.4.md
        docs/TDD_HEADER_RIBBON_0.6.3.md
        docs/TDD_WORKSPACE_0.6.2.md
        docs/CLICK_AUDIT_0.6.2.md
        docs/TDD_HEADER_FOOTER_EVIDENCE.md
        docs/TDD_WORKSPACE_FEEDBACK_EVIDENCE.md
        docs/TDD_LAYOUT_EVIDENCE.md
        docs/TDD_MATH_EVIDENCE.md
        docs/TDD_SAVE_EVIDENCE.md
        docs/TDD_WORKSPACE_EVIDENCE.md
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

    add_executable(academic_core_tests tests/academic_core_tests.cpp)
    target_link_libraries(academic_core_tests PRIVATE docxstudio_core)
    docxstudio_warnings(academic_core_tests)
    add_test(NAME academic_core_tests COMMAND academic_core_tests)

    add_executable(academic_recovery_tests tests/academic_recovery_tests.cpp src/app/RecoveryCodec.cpp)
    target_compile_definitions(academic_recovery_tests PRIVATE OWL_DOCS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(academic_recovery_tests PRIVATE docxstudio_core nlohmann_json::nlohmann_json)
    docxstudio_warnings(academic_recovery_tests)
    add_test(NAME academic_recovery_tests COMMAND academic_recovery_tests)

    add_executable(academic_ooxml_tests tests/academic_ooxml_tests.cpp)
    target_link_libraries(academic_ooxml_tests PRIVATE docxstudio_ooxml)
    docxstudio_warnings(academic_ooxml_tests)
    add_test(NAME academic_ooxml_tests COMMAND academic_ooxml_tests)

    add_executable(codex_tests tests/codex_tests.cpp)
    target_link_libraries(codex_tests PRIVATE docxstudio_codex)
    docxstudio_warnings(codex_tests)
    add_test(NAME codex_tests COMMAND codex_tests)

    add_executable(math_tests tests/math_tests.cpp)
    target_link_libraries(math_tests PRIVATE docxstudio_math)
    docxstudio_warnings(math_tests)
    add_test(NAME math_tests COMMAND math_tests)

    add_executable(math_roundtrip_tests tests/math_roundtrip_tests.cpp)
    target_link_libraries(math_roundtrip_tests PRIVATE docxstudio_ooxml)
    docxstudio_warnings(math_roundtrip_tests)
    add_test(NAME math_roundtrip_tests COMMAND math_roundtrip_tests)

    if(DOCXSTUDIO_BUILD_APP)
        add_executable(table_insert_picker_tests tests/table_insert_picker_tests.cpp)
        target_sources(table_insert_picker_tests PRIVATE src/app/TableInsertDialog.cpp include/docxstudio/app/TableInsertDialog.h)
        target_include_directories(table_insert_picker_tests PRIVATE include)
        target_link_libraries(table_insert_picker_tests PRIVATE Qt6::Widgets Qt6::Test)
        docxstudio_warnings(table_insert_picker_tests)
        add_test(NAME table_insert_picker_tests COMMAND table_insert_picker_tests)
        set_tests_properties(table_insert_picker_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(color_palette_tests tests/color_palette_tests.cpp)
        target_sources(color_palette_tests PRIVATE src/app/ColorPaletteWidget.cpp include/docxstudio/app/ColorPaletteWidget.h)
        set_target_properties(color_palette_tests PROPERTIES AUTOMOC ON)
        target_include_directories(color_palette_tests PRIVATE include)
        target_link_libraries(color_palette_tests PRIVATE Qt6::Widgets Qt6::Test)
        docxstudio_warnings(color_palette_tests)
        add_test(NAME color_palette_tests COMMAND color_palette_tests)
        set_tests_properties(color_palette_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(header_margin_tests tests/header_margin_tests.cpp)
        target_link_libraries(header_margin_tests PRIVATE docxstudio_app Qt6::Test)
        docxstudio_warnings(header_margin_tests)
        add_test(NAME header_margin_tests COMMAND header_margin_tests)
        set_tests_properties(header_margin_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(feedback_workflow_tests tests/feedback_workflow_tests.cpp)
        target_link_libraries(feedback_workflow_tests PRIVATE docxstudio_app Qt6::Test)
        docxstudio_warnings(feedback_workflow_tests)
        add_test(NAME feedback_workflow_tests COMMAND feedback_workflow_tests)
        set_tests_properties(feedback_workflow_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(table_image_ooxml_tests tests/table_image_ooxml_tests.cpp)
        target_link_libraries(table_image_ooxml_tests PRIVATE docxstudio_ooxml Qt6::Gui)
        docxstudio_warnings(table_image_ooxml_tests)
        add_test(NAME table_image_ooxml_tests COMMAND table_image_ooxml_tests)
        add_executable(table_format_ooxml_tests tests/table_format_ooxml_tests.cpp)
        target_link_libraries(table_format_ooxml_tests PRIVATE docxstudio_ooxml)
        docxstudio_warnings(table_format_ooxml_tests)
        add_test(NAME table_format_ooxml_tests COMMAND table_format_ooxml_tests)
        add_executable(table_layout_core_tests tests/table_layout_core_tests.cpp)
        target_link_libraries(table_layout_core_tests PRIVATE docxstudio_core)
        docxstudio_warnings(table_layout_core_tests)
        add_test(NAME table_layout_core_tests COMMAND table_layout_core_tests)
        add_executable(table_layout_recovery_tests tests/table_layout_recovery_tests.cpp src/app/RecoveryCodec.cpp)
        target_link_libraries(table_layout_recovery_tests PRIVATE docxstudio_core docxstudio_raster Qt6::Core)
        docxstudio_warnings(table_layout_recovery_tests)
        add_test(NAME table_layout_recovery_tests COMMAND table_layout_recovery_tests)
        add_executable(table_image_core_tests tests/table_image_core_tests.cpp)
        target_link_libraries(table_image_core_tests PRIVATE docxstudio_core)
        docxstudio_warnings(table_image_core_tests)
        add_test(NAME table_image_core_tests COMMAND table_image_core_tests)
        add_executable(table_image_ui_tests tests/table_image_ui_tests.cpp)
        target_link_libraries(table_image_ui_tests PRIVATE docxstudio_app Qt6::Test)
        target_compile_definitions(table_image_ui_tests PRIVATE DOCXSTUDIO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        docxstudio_warnings(table_image_ui_tests)
        add_test(NAME table_image_ui_tests COMMAND table_image_ui_tests)
        set_tests_properties(table_image_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        foreach(OWL_DOCS_CELL_IMAGE_CASE recovery selection clipboard docx codex)
            add_test(NAME table_image_${OWL_DOCS_CELL_IMAGE_CASE}_tests
                COMMAND table_image_ui_tests --${OWL_DOCS_CELL_IMAGE_CASE}-only)
            set_tests_properties(table_image_${OWL_DOCS_CELL_IMAGE_CASE}_tests PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
        endforeach()
        add_executable(academic_layout_tests tests/academic_layout_tests.cpp)
        target_link_libraries(academic_layout_tests PRIVATE docxstudio_app)
        docxstudio_warnings(academic_layout_tests)
        add_test(NAME academic_layout_tests COMMAND academic_layout_tests)
        set_tests_properties(academic_layout_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(academic_ui_tests tests/academic_ui_tests.cpp)
        target_link_libraries(academic_ui_tests PRIVATE docxstudio_app)
        docxstudio_warnings(academic_ui_tests)
        add_test(NAME academic_ui_tests COMMAND academic_ui_tests)
        set_tests_properties(academic_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(academic_clipboard_tests tests/academic_clipboard_tests.cpp)
        target_link_libraries(academic_clipboard_tests PRIVATE docxstudio_app)
        docxstudio_warnings(academic_clipboard_tests)
        add_test(NAME academic_clipboard_tests COMMAND academic_clipboard_tests)
        set_tests_properties(academic_clipboard_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(header_footer_ui_tests tests/header_footer_ui_tests.cpp)
        target_link_libraries(header_footer_ui_tests PRIVATE docxstudio_app)
        docxstudio_warnings(header_footer_ui_tests)
        add_test(NAME header_footer_ui_tests COMMAND header_footer_ui_tests)
        set_tests_properties(header_footer_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(header_footer_clipboard_tests tests/header_footer_clipboard_tests.cpp)
        target_link_libraries(header_footer_clipboard_tests PRIVATE docxstudio_app)
        docxstudio_warnings(header_footer_clipboard_tests)
        add_test(NAME header_footer_clipboard_tests COMMAND header_footer_clipboard_tests)
        set_tests_properties(header_footer_clipboard_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_test(NAME header_footer_keyboard_tests COMMAND header_footer_clipboard_tests --keyboard-only)
        set_tests_properties(header_footer_keyboard_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(header_footer_toolbar_tests tests/header_footer_toolbar_tests.cpp resources/owl-docs.qrc)
        set_target_properties(header_footer_toolbar_tests PROPERTIES AUTORCC ON)
        target_link_libraries(header_footer_toolbar_tests PRIVATE docxstudio_app Qt6::Test)
        docxstudio_warnings(header_footer_toolbar_tests)
        add_test(NAME header_footer_toolbar_tests COMMAND header_footer_toolbar_tests)
        set_tests_properties(header_footer_toolbar_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(table_editing_ui_tests tests/table_editing_ui_tests.cpp)
        target_link_libraries(table_editing_ui_tests PRIVATE docxstudio_app Qt6::Test)
        docxstudio_warnings(table_editing_ui_tests)
        add_test(NAME table_editing_ui_tests COMMAND table_editing_ui_tests)
        set_tests_properties(table_editing_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(table_layout_clipboard_tests tests/table_layout_clipboard_tests.cpp)
        target_link_libraries(table_layout_clipboard_tests PRIVATE docxstudio_app)
        docxstudio_warnings(table_layout_clipboard_tests)
        add_test(NAME table_layout_clipboard_tests COMMAND table_layout_clipboard_tests)
        set_tests_properties(table_layout_clipboard_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(table_format_workflow_tests tests/table_format_workflow_tests.cpp)
        target_link_libraries(table_format_workflow_tests PRIVATE docxstudio_app Qt6::Test)
        docxstudio_warnings(table_format_workflow_tests)
        add_test(NAME table_format_workflow_tests COMMAND table_format_workflow_tests)
        set_tests_properties(table_format_workflow_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(table_cell_formatting_tests tests/table_cell_formatting_tests.cpp)
        target_link_libraries(table_cell_formatting_tests PRIVATE docxstudio_app)
        docxstudio_warnings(table_cell_formatting_tests)
        add_test(NAME table_cell_formatting_tests COMMAND table_cell_formatting_tests)
        set_tests_properties(table_cell_formatting_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(table_cell_render_tests tests/table_cell_render_tests.cpp)
        target_link_libraries(table_cell_render_tests PRIVATE docxstudio_app)
        docxstudio_warnings(table_cell_render_tests)
        add_test(NAME table_cell_render_tests COMMAND table_cell_render_tests)
        set_tests_properties(table_cell_render_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(story_wrap_workflow_tests tests/story_wrap_workflow_tests.cpp)
        target_link_libraries(story_wrap_workflow_tests PRIVATE docxstudio_app)
        docxstudio_warnings(story_wrap_workflow_tests)
        add_test(NAME story_wrap_workflow_tests COMMAND story_wrap_workflow_tests)
        set_tests_properties(story_wrap_workflow_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(header_footer_ime_tests tests/header_footer_ime_tests.cpp)
        target_link_libraries(header_footer_ime_tests PRIVATE docxstudio_app)
        docxstudio_warnings(header_footer_ime_tests)
        add_test(NAME header_footer_ime_tests COMMAND header_footer_ime_tests)
        set_tests_properties(header_footer_ime_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(equation_editor_tests tests/equation_editor_tests.cpp
            src/app/EquationEditorDialog.cpp src/app/MathLayout.cpp)
        target_link_libraries(equation_editor_tests PRIVATE docxstudio_math Qt6::Widgets)
        docxstudio_warnings(equation_editor_tests)
        add_test(NAME equation_editor_tests COMMAND equation_editor_tests)
        set_tests_properties(equation_editor_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    if(TARGET docxstudio_app)
        add_executable(application_identity_tests tests/application_identity_tests.cpp)
        target_link_libraries(application_identity_tests PRIVATE Qt6::Widgets)
        target_include_directories(application_identity_tests PRIVATE include)
        target_compile_definitions(application_identity_tests PRIVATE OWL_DOCS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        docxstudio_warnings(application_identity_tests)
        add_test(NAME application_identity_tests COMMAND application_identity_tests)
        set_tests_properties(application_identity_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(legacy_style_workflow_tests tests/legacy_style_workflow_tests.cpp)
        target_link_libraries(legacy_style_workflow_tests PRIVATE docxstudio_app)
        target_compile_definitions(legacy_style_workflow_tests PRIVATE OWL_DOCS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        docxstudio_warnings(legacy_style_workflow_tests)
        add_test(NAME legacy_style_workflow_tests COMMAND legacy_style_workflow_tests)
        set_tests_properties(legacy_style_workflow_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-legacy-style-workflow-tests")
        add_executable(academic_document_workflow_tests tests/academic_document_workflow_tests.cpp)
        target_link_libraries(academic_document_workflow_tests PRIVATE docxstudio_app)
        docxstudio_warnings(academic_document_workflow_tests)
        add_test(NAME academic_document_workflow_tests COMMAND academic_document_workflow_tests)
        set_tests_properties(academic_document_workflow_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-academic-workflow-tests")
        add_executable(academic_tool_tests tests/academic_tool_tests.cpp)
        target_link_libraries(academic_tool_tests PRIVATE docxstudio_app)
        docxstudio_warnings(academic_tool_tests)
        add_test(NAME academic_tool_tests COMMAND academic_tool_tests)
        set_tests_properties(academic_tool_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-academic-tool-tests")
        add_executable(math_layout_tests tests/math_layout_tests.cpp)
        target_link_libraries(math_layout_tests PRIVATE docxstudio_app)
        docxstudio_warnings(math_layout_tests)
        add_test(NAME math_layout_tests COMMAND math_layout_tests)
        set_tests_properties(math_layout_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-math-layout-tests")
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ooxml_tests.cpp")
        add_executable(story_ooxml_tests tests/story_ooxml_tests.cpp)
        target_link_libraries(story_ooxml_tests PRIVATE docxstudio_ooxml)
        target_compile_definitions(story_ooxml_tests PRIVATE OWL_DOCS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        docxstudio_warnings(story_ooxml_tests)
        add_test(NAME story_ooxml_tests COMMAND story_ooxml_tests)
        add_executable(legacy_style_profile_tests tests/legacy_style_profile_tests.cpp)
        target_link_libraries(legacy_style_profile_tests PRIVATE docxstudio_ooxml)
        target_compile_definitions(legacy_style_profile_tests PRIVATE OWL_DOCS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        docxstudio_warnings(legacy_style_profile_tests)
        add_test(NAME legacy_style_profile_tests COMMAND legacy_style_profile_tests)
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

        add_executable(excalidraw_scene_tests tests/excalidraw_scene_tests.cpp src/app/ExcalidrawScene.cpp)
        target_include_directories(excalidraw_scene_tests PRIVATE include)
        target_link_libraries(excalidraw_scene_tests PRIVATE Qt6::Core)
        docxstudio_warnings(excalidraw_scene_tests)
        add_test(NAME excalidraw_scene_tests COMMAND excalidraw_scene_tests)

        add_executable(excalidraw_figure_tests
            tests/excalidraw_figure_tests.cpp)
        target_link_libraries(excalidraw_figure_tests PRIVATE docxstudio_app)
        docxstudio_warnings(excalidraw_figure_tests)
        add_test(NAME excalidraw_figure_tests COMMAND excalidraw_figure_tests)
        set_tests_properties(excalidraw_figure_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-excalidraw-figure-tests")

        add_executable(codex_figure_patch_tests tests/codex_figure_patch_tests.cpp)
        target_link_libraries(codex_figure_patch_tests PRIVATE docxstudio_app Qt6::Test)
        docxstudio_warnings(codex_figure_patch_tests)
        add_test(NAME codex_figure_patch_tests COMMAND codex_figure_patch_tests)
        add_test(NAME codex_figure_reconstruction_tests COMMAND codex_figure_patch_tests --reconstruction)
        set_tests_properties(codex_figure_patch_tests codex_figure_reconstruction_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-codex-figure-patch-tests")

        add_executable(codex_figure_geometry_tests tests/codex_figure_geometry_tests.cpp)
        target_link_libraries(codex_figure_geometry_tests PRIVATE docxstudio_app Qt6::Test)
        docxstudio_warnings(codex_figure_geometry_tests)
        add_test(NAME codex_figure_geometry_tests COMMAND codex_figure_geometry_tests)
        set_tests_properties(codex_figure_geometry_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-codex-figure-geometry-tests")

        add_executable(image_payload_preview_tests tests/image_payload_preview_tests.cpp)
        target_link_libraries(image_payload_preview_tests PRIVATE docxstudio_app Qt6::Test)
        docxstudio_warnings(image_payload_preview_tests)
        add_test(NAME image_payload_preview_tests COMMAND image_payload_preview_tests)
        set_tests_properties(image_payload_preview_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-image-payload-preview-tests")

        add_executable(excalidraw_process_tests tests/excalidraw_process_tests.cpp)
        target_link_libraries(excalidraw_process_tests PRIVATE docxstudio_app Qt6::Test)
        docxstudio_warnings(excalidraw_process_tests)
        add_test(NAME excalidraw_process_tests COMMAND excalidraw_process_tests)
        add_test(NAME excalidraw_process_timeout_tests COMMAND excalidraw_process_tests --timeout)
        set_tests_properties(excalidraw_process_tests excalidraw_process_timeout_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-excalidraw-process-tests"
            TIMEOUT 60)

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

        add_executable(codex_preview_state_tests tests/codex_preview_state_tests.cpp)
        target_link_libraries(codex_preview_state_tests PRIVATE docxstudio_app)
        docxstudio_warnings(codex_preview_state_tests)
        add_test(NAME codex_preview_state_tests COMMAND codex_preview_state_tests)
        set_tests_properties(codex_preview_state_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-codex-preview-tests")

        add_executable(codex_numbering_refresh_tests tests/codex_numbering_refresh_tests.cpp)
        target_link_libraries(codex_numbering_refresh_tests PRIVATE docxstudio_app)
        docxstudio_warnings(codex_numbering_refresh_tests)
        add_test(NAME codex_numbering_refresh_tests COMMAND codex_numbering_refresh_tests)
        set_tests_properties(codex_numbering_refresh_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-codex-numbering-tests")

        add_executable(codex_authoring_workflow_tests tests/codex_authoring_workflow_tests.cpp)
        target_link_libraries(codex_authoring_workflow_tests PRIVATE docxstudio_app Qt6::Test)
        docxstudio_warnings(codex_authoring_workflow_tests)
        add_test(NAME codex_authoring_workflow_tests COMMAND codex_authoring_workflow_tests)
        set_tests_properties(codex_authoring_workflow_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-codex-authoring-tests")

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
# exact experimental fields used by OwlDocs against app-server's generated
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

if(BUILD_TESTING)
    find_program(OWL_DOCS_FIGURE_TEST_NODE NAMES node nodejs)
    if(OWL_DOCS_FIGURE_TEST_NODE)
        add_test(NAME excalidraw_figure_protocol_tests
            COMMAND "${OWL_DOCS_FIGURE_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/excalidraw-figure-editor/scripts/figure-protocol.test.mjs")
        add_test(NAME excalidraw_native_scene_patch_tests
            COMMAND "${OWL_DOCS_FIGURE_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/excalidraw-figure-editor/scripts/native-scene-patch.test.mjs")
        add_test(NAME excalidraw_figure_export_options_tests
            COMMAND "${OWL_DOCS_FIGURE_TEST_NODE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/excalidraw-figure-editor/scripts/figure-export-options.test.mjs")
        set_tests_properties(excalidraw_figure_protocol_tests excalidraw_native_scene_patch_tests excalidraw_figure_export_options_tests
            PROPERTIES LABELS "excalidraw;protocol" TIMEOUT 30)
    endif()
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "owl-docs")
set(CPACK_PACKAGE_VENDOR "OwlDocs")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Offline word processor with optional Codex assistance")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
    "Offline DOCX word processor with optional Codex assistance.\n OwlDocs provides local document editing, PDF export, and an optional\n user-enabled Codex chat experience.")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "OwlDocs")
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

# Workspace UI acceptance tests and the shared desktop visual system.
if(TARGET docxstudio_app AND BUILD_TESTING)
    add_executable(workspace_ui_tests tests/workspace_ui_tests.cpp)
    target_link_libraries(workspace_ui_tests PRIVATE docxstudio_app Qt6::Test)
    docxstudio_warnings(workspace_ui_tests)
    add_test(NAME workspace_ui_tests COMMAND workspace_ui_tests)
    set_tests_properties(workspace_ui_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_DATA_HOME=/tmp/owl-docs-workspace-tests")
endif()

if(TARGET docxstudio_app AND BUILD_TESTING)
    add_executable(command_registry_tests tests/command_registry_tests.cpp
        src/app/CommandRegistry.cpp include/docxstudio/app/CommandRegistry.h)
    set_target_properties(command_registry_tests PROPERTIES AUTOMOC ON)
    target_include_directories(command_registry_tests PRIVATE include)
    target_link_libraries(command_registry_tests PRIVATE Qt6::Widgets Qt6::Test)
    docxstudio_warnings(command_registry_tests)
    add_test(NAME command_registry_tests COMMAND command_registry_tests)
    set_tests_properties(command_registry_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(editor_semantic_operations_tests tests/editor_semantic_operations_tests.cpp
        src/app/EditorSemanticOperations.cpp src/app/RasterDecoder.cpp)
    target_include_directories(editor_semantic_operations_tests PRIVATE include)
    target_link_libraries(editor_semantic_operations_tests PRIVATE docxstudio_core docxstudio_codex Qt6::Gui)
    docxstudio_warnings(editor_semantic_operations_tests)
    add_test(NAME editor_semantic_operations_tests COMMAND editor_semantic_operations_tests)
    set_tests_properties(editor_semantic_operations_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(list_operation_planning_tests tests/list_operation_planning_tests.cpp)
    target_link_libraries(list_operation_planning_tests PRIVATE docxstudio_app)
    docxstudio_warnings(list_operation_planning_tests)
    add_test(NAME list_operation_planning_tests COMMAND list_operation_planning_tests)
    set_tests_properties(list_operation_planning_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=/tmp/owl-docs-list-planning-tests")
endif()

if(TARGET docxstudio_app AND BUILD_TESTING)
    add_executable(workspace_controls_tests tests/workspace_controls_tests.cpp
        src/app/CommandRegistry.cpp src/app/RibbonWidget.cpp
        src/app/ColorPaletteWidget.cpp include/docxstudio/app/ColorPaletteWidget.h
        src/app/FontFamilyPicker.cpp src/app/NavigationDock.cpp src/app/ChatDock.cpp
        include/docxstudio/app/CommandRegistry.h include/docxstudio/app/RibbonWidget.h
        include/docxstudio/app/NavigationDock.h include/docxstudio/app/ChatDock.h)
    set_target_properties(workspace_controls_tests PROPERTIES AUTOMOC ON)
    target_include_directories(workspace_controls_tests PRIVATE include)
    target_link_libraries(workspace_controls_tests PRIVATE docxstudio_core Qt6::Widgets Qt6::Test)
    docxstudio_warnings(workspace_controls_tests)
    add_test(NAME workspace_controls_tests COMMAND workspace_controls_tests)
    set_tests_properties(workspace_controls_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(TARGET docxstudio_app)
    target_sources(docxstudio_app PRIVATE src/app/AppTheme.cpp)
endif()
if(TARGET workspace_controls_tests)
    target_sources(workspace_controls_tests PRIVATE src/app/AppTheme.cpp)
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(save_outcome_tests tests/save_outcome_tests.cpp)
    target_link_libraries(save_outcome_tests PRIVATE docxstudio_ooxml)
    target_link_options(save_outcome_tests PRIVATE "-Wl,--wrap=fsync")
    docxstudio_warnings(save_outcome_tests)
    add_test(NAME save_outcome_tests COMMAND save_outcome_tests)
endif()

if(TARGET workspace_ui_tests)
    target_sources(workspace_ui_tests PRIVATE resources/owl-docs.qrc)
    set_target_properties(workspace_ui_tests PROPERTIES AUTORCC ON)
endif()
