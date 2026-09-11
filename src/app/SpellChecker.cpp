#include "docxstudio/app/SpellChecker.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

#include <hunspell/hunspell.hxx>

namespace docxstudio::app {

SpellChecker::SpellChecker() {
    const QString aff = QStringLiteral("/usr/share/hunspell/en_US.aff");
    const QString dic = QStringLiteral("/usr/share/hunspell/en_US.dic");
    if (QFile::exists(aff) && QFile::exists(dic)) {
        hunspell_ = std::make_unique<Hunspell>(aff.toUtf8().constData(), dic.toUtf8().constData());
    }

    const auto stateDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(stateDir);
    personalPath_ = stateDir + QStringLiteral("/personal-dictionary.txt");
    QFile file(personalPath_);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const auto word = normalized(stream.readLine());
            if (!word.isEmpty()) {
                personal_.insert(word);
                if (hunspell_) {
                    hunspell_->add(word.toUtf8().constData());
                }
            }
        }
    }
}

SpellChecker::~SpellChecker() = default;

bool SpellChecker::available() const { return static_cast<bool>(hunspell_); }

QString SpellChecker::normalized(const QString& word) const {
    return word.trimmed().toCaseFolded();
}

bool SpellChecker::isCorrect(const QString& word) const {
    const auto key = normalized(word);
    if (key.isEmpty() || ignored_.contains(key) || personal_.contains(key)) {
        return true;
    }
    return !hunspell_ || hunspell_->spell(word.toUtf8().toStdString());
}

QStringList SpellChecker::suggestions(const QString& word, int limit) const {
    QStringList result;
    if (!hunspell_) {
        return result;
    }
    const auto raw = hunspell_->suggest(word.toUtf8().constData());
    for (const auto& candidate : raw) {
        result.push_back(QString::fromUtf8(candidate));
        if (result.size() >= limit) {
            break;
        }
    }
    return result;
}

void SpellChecker::ignoreAll(const QString& word) {
    ignored_.insert(normalized(word));
}

bool SpellChecker::addToPersonalDictionary(const QString& word) {
    const auto key = normalized(word);
    if (key.isEmpty() || personal_.contains(key)) {
        return false;
    }
    QFile file(personalPath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    QTextStream stream(&file);
    stream << word.trimmed() << '\n';
    personal_.insert(key);
    if (hunspell_) {
        hunspell_->add(word.trimmed().toUtf8().constData());
    }
    return true;
}

}  // namespace docxstudio::app
