#pragma once

#include "docxstudio/app/ChatStore.h"
#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/EditorPreferences.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/codex/types.hpp"

#include <QMainWindow>
#include <QPointer>

#include <memory>
#include <unordered_map>

class QLabel;
class QLockFile;
class QMenu;
class QColorDialog;
class QColor;
class QTabWidget;
class QTimer;

namespace docxstudio::core { class Document; }
namespace docxstudio::ooxml { class DocxDocument; }

namespace docxstudio::app {

class ChatDock;
class CodexController;
class DocumentCanvas;
class RibbonWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openPath(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct TabState;

    void registerCommands();
    void buildMenus();
    void connectRibbon();
    void applyTextColor(const QColor& color);
    void applyHighlightColor(const QColor& color);
    void clearHighlight();
    void showLiveColorPicker(bool highlight);
    void restoreCanvasFocus(DocumentCanvas* canvas);
    DocumentCanvas* createDocumentTab(core::Document document,
                                      std::unique_ptr<TabState> state,
                                      const QString& title);
    DocumentCanvas* activeCanvas() const;
    TabState* activeState() const;
    TabState* stateFor(DocumentCanvas* canvas) const;
    void newDocument();
    void openDocument();
    bool saveDocument(bool saveAs);
    bool saveCanvas(DocumentCanvas* canvas, bool saveAs);
    bool maybeCloseTab(int index);
    void closeTab(int index);
    void exportPdf();
    void printPreview();
    void printDocument();
    void showFindReplace();
    void showCommandPalette();
    void showEditorOptions();
    void applyEditorPreferences();
    void showListProperties();
    void insertTable();
    void insertEquation();
    void insertImage();
    void showCompatibilityReport();
    void updateWindowTitle();
    void updateTabTitle(DocumentCanvas* canvas);
    void addRecentFile(const QString& path);
    void rebuildRecentMenu();
    QString documentKey() const;
    QString documentKeyFor(DocumentCanvas* canvas) const;
    DocumentCanvas* canvasForDocumentKey(const QString& key) const;
    codex::Json handleEditorTool(const QString& documentKey,
                                 const QString& tool,
                                 const codex::Json& arguments,
                                 QString& error);
    void loadChatForActiveDocument();
    void checkpointCanvas(DocumentCanvas* canvas);
    void checkpointModifiedDocuments();
    void deleteRecoveryFor(DocumentCanvas* canvas);
    void restoreRecoveryJournals();

    CommandRegistry commands_;
    EditorPreferences editorPreferences_;
    SpellChecker spelling_;
    ChatStore chatStore_;
    std::unique_ptr<QLockFile> instanceLock_;
    QTabWidget* tabs_{};
    RibbonWidget* ribbon_{};
    ChatDock* chat_{};
    CodexController* codex_{};
    QLabel* pageStatus_{};
    QLabel* saveStatus_{};
    QMenu* recentMenu_{};
    QTimer* recoveryDebounce_{};
    QTimer* recoveryPeriodic_{};
    QPointer<QColorDialog> textColorPicker_;
    QPointer<QColorDialog> highlightColorPicker_;
    bool recoveryOwner_{false};
    QString assistantForStore_;
    QString assistantDocumentKey_;
    QString previewDocumentKey_;
    QString previewLabel_;
    QString previewSummary_;
    std::unordered_map<DocumentCanvas*, std::unique_ptr<TabState>> states_;
};

}  // namespace docxstudio::app
