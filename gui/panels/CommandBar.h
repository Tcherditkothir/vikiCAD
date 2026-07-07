#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace viki {

// AutoCAD-style command bar: scrollback history, prompt label, input line.
class CommandBar : public QWidget {
    Q_OBJECT
public:
    explicit CommandBar(QWidget* parent = nullptr);

    void appendHistory(const QString& text);
    void setPrompt(const QString& prompt);
    void focusInput();

signals:
    // Emitted on Enter; empty line means "Enter" (finish current command).
    void commandEntered(const QString& line);

private:
    QPlainTextEdit* m_history = nullptr;
    QLabel* m_prompt = nullptr;
    QLineEdit* m_input = nullptr;
};

} // namespace viki
