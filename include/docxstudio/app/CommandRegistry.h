#pragma once

#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>

#include <functional>

class QAction;

namespace docxstudio::app {

class CommandRegistry final : public QObject {
    Q_OBJECT

public:
    explicit CommandRegistry(QObject* parent = nullptr);

    QAction* add(const QString& id,
                 const QString& text,
                 const QKeySequence& shortcut,
                 std::function<void()> handler,
                 bool checkable = false);
    QAction* add(const QString& id,
                 const QString& text,
                 QKeySequence::StandardKey shortcut,
                 std::function<void()> handler,
                 bool checkable = false);
    QAction* action(const QString& id) const;
    QList<QAction*> actions() const;
    QStringList search(const QString& query) const;

signals:
    void invoked(const QString& id);

private:
    QHash<QString, QAction*> actions_;
};

}  // namespace docxstudio::app
