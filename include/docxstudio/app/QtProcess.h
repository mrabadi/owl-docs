#pragma once

#include "docxstudio/codex/transport.hpp"

#include <QObject>

class QProcess;

namespace docxstudio::app {

class QtProcess final : public QObject, public codex::Process {
    Q_OBJECT

public:
    explicit QtProcess(QObject* parent = nullptr);
    ~QtProcess() override;

    bool start(const codex::ProcessSpec& spec,
               codex::ProcessCallbacks callbacks,
               std::string& error) override;
    bool writeStandardInput(std::string_view bytes, std::string& error) override;
    void stop() noexcept override;

private:
    QString resolveProgram(const std::string& requested) const;

    QProcess* process_{};
    codex::ProcessCallbacks callbacks_;
};

}  // namespace docxstudio::app
