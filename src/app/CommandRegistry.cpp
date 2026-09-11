#include "docxstudio/app/CommandRegistry.h"

#include <QAction>

#include <algorithm>

namespace docxstudio::app {

CommandRegistry::CommandRegistry(QObject* parent) : QObject(parent) {}

QAction* CommandRegistry::add(const QString& id,
                              const QString& text,
                              const QKeySequence& shortcut,
                              std::function<void()> handler,
                              bool checkable) {
    if (actions_.contains(id)) {
        return actions_.value(id);
    }

    auto* created = new QAction(text, this);
    created->setObjectName(id);
    created->setCheckable(checkable);
    if (!shortcut.isEmpty()) {
        created->setShortcut(shortcut);
        created->setShortcutContext(Qt::WindowShortcut);
    }
    connect(created, &QAction::triggered, this,
            [this, id, handler = std::move(handler)]() {
                handler();
                emit invoked(id);
            });
    actions_.insert(id, created);
    return created;
}

QAction* CommandRegistry::add(const QString& id,
                              const QString& text,
                              QKeySequence::StandardKey shortcut,
                              std::function<void()> handler,
                              bool checkable) {
    auto* action = add(id, text, QKeySequence(), std::move(handler), checkable);
    action->setShortcuts(QKeySequence::keyBindings(shortcut));
    action->setShortcutContext(Qt::WindowShortcut);
    return action;
}

QAction* CommandRegistry::action(const QString& id) const {
    return actions_.value(id, nullptr);
}

QList<QAction*> CommandRegistry::actions() const {
    return actions_.values();
}

QStringList CommandRegistry::search(const QString& query) const {
    struct Match {
        int score;
        QString id;
    };
    QList<Match> matches;
    const auto needle = query.trimmed().toCaseFolded();
    for (auto it = actions_.cbegin(); it != actions_.cend(); ++it) {
        const auto haystack = (it.key() + QLatin1Char(' ') + it.value()->text()).toCaseFolded();
        const auto position = haystack.indexOf(needle);
        if (needle.isEmpty() || position >= 0) {
            matches.push_back({position < 0 ? 0 : static_cast<int>(position), it.key()});
        }
    }
    std::sort(matches.begin(), matches.end(), [](const Match& left, const Match& right) {
        return left.score == right.score ? left.id < right.id : left.score < right.score;
    });
    QStringList result;
    for (const auto& match : matches) {
        result.push_back(match.id);
    }
    return result;
}

}  // namespace docxstudio::app
