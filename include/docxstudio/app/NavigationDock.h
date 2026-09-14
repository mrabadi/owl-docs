#pragma once

#include <QDockWidget>
#include <QList>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabBar;

namespace docxstudio::app {

// Presentation-only data for one match in the Navigation pane. The document
// layer retains ownership of semantic positions; the row index emitted by the
// dock identifies the corresponding entry in the latest result set.
struct NavigationResult {
    QString title;
    QString snippet;
    QString accessibleText;

    bool operator==(const NavigationResult&) const = default;
};

// Modeless Find/Replace surface. This widget deliberately knows nothing about
// DocumentCanvas or core positions, so MainWindow can bind it to whichever
// document tab is active without introducing document state into the shell.
class NavigationDock final : public QDockWidget {
    Q_OBJECT

public:
    enum class Mode {
        find,
        replace,
    };
    Q_ENUM(Mode)

    explicit NavigationDock(QWidget* parent = nullptr);

    Mode mode() const noexcept { return mode_; }
    QString query() const;
    QString replacement() const;
    bool matchCase() const noexcept;
    bool wholeWords() const noexcept;
    int resultCount() const noexcept;
    int currentResultIndex() const noexcept;

    void setMode(Mode mode);
    void setQuery(const QString& query);
    void setReplacement(const QString& replacement);
    void setMatchCase(bool enabled);
    void setWholeWords(bool enabled);
    void setReplacementLocked(bool locked);
    void setSearchResults(const QList<NavigationResult>& results,
                          int currentIndex = -1);
    void setCurrentResultIndex(int index);
    void clearSearchResults();
    void focusQuery(bool selectAll = true);

signals:
    // Emitted for both query edits and option changes. Consumers can debounce
    // expensive searches while still providing a live-results experience.
    void queryChanged(const QString& query, bool matchCase, bool wholeWords);
    void replacementChanged(const QString& replacement);
    void previousRequested(const QString& query, bool matchCase,
                           bool wholeWords);
    void nextRequested(const QString& query, bool matchCase,
                       bool wholeWords);
    void resultActivated(int index);
    void replaceRequested(const QString& query, const QString& replacement,
                          bool matchCase, bool wholeWords);
    void replaceAllRequested(const QString& query,
                             const QString& replacement,
                             bool matchCase, bool wholeWords);
    void modeChanged(docxstudio::app::NavigationDock::Mode mode);
    void dismissRequested();

private:
    void emitQueryChanged();
    void updateModePresentation();
    void updateResultPresentation();
    void updateActionAvailability();

    Mode mode_{Mode::find};
    QTabBar* modes_{};
    QLineEdit* query_{};
    QWidget* replacementRow_{};
    QLineEdit* replacement_{};
    QCheckBox* matchCase_{};
    QCheckBox* wholeWords_{};
    QPushButton* previous_{};
    QPushButton* next_{};
    QLabel* resultCount_{};
    QListWidget* results_{};
    QWidget* replaceActions_{};
    QPushButton* replace_{};
    QPushButton* replaceAll_{};
    bool replacementLocked_{false};
};

}  // namespace docxstudio::app
