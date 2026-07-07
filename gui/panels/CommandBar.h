#pragma once

#include <QWidget>

class QLineEdit;
class QPlainTextEdit;

namespace viki {

// AutoCAD-style command bar: scrollback history above a single input line.
class CommandBar : public QWidget {
    Q_OBJECT
public:
    explicit CommandBar(QWidget* parent = nullptr);

    void appendHistory(const QString& text);

signals:
    void commandEntered(const QString& line);

private:
    QPlainTextEdit* m_history = nullptr;
    QLineEdit* m_input = nullptr;
};

} // namespace viki
