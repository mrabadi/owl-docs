#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

class Hunspell;

namespace docxstudio::app {

class SpellChecker final {
public:
    SpellChecker();
    ~SpellChecker();

    bool available() const;
    bool isCorrect(const QString& word) const;
    QStringList suggestions(const QString& word, int limit = 8) const;
    void ignoreAll(const QString& word);
    bool addToPersonalDictionary(const QString& word);

private:
    QString normalized(const QString& word) const;
    std::unique_ptr<Hunspell> hunspell_;
    QSet<QString> ignored_;
    QSet<QString> personal_;
    QString personalPath_;
};

}  // namespace docxstudio::app
