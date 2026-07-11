#pragma once

#include <functional>

#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QCompleter;

namespace viki {

// AutoCAD-style command bar: scrollback history, prompt label, input line.
class CommandBar : public QWidget {
    Q_OBJECT
public:
    explicit CommandBar(QWidget* parent = nullptr);

    void appendHistory(const QString& text);
    void setPrompt(const QString& prompt);
    void focusInput();
    // Command names/aliases for the autocompletion dropdown.
    void setCompletions(const QStringList& names);
    // AutoCAD-style: typing anywhere lands here (focus + insert the chars).
    void beginTyping(const QString& seed);
    // When the predicate returns true, Space submits like Enter (it stays a
    // real space while a command is asking for free text).
    void setSpaceSubmits(std::function<bool()> predicate)
    {
        m_spaceSubmits = std::move(predicate);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    // Emitted on Enter; empty line means "Enter" (finish current command).
    void commandEntered(const QString& line);
    // ESC pressed in the input (cancel the active command).
    void cancelRequested();

private:
    void submitLine();
    void moveCompleterPopupToAnchor();

    QPlainTextEdit* m_history = nullptr;
    QLabel* m_prompt = nullptr;
    QLineEdit* m_input = nullptr;
    QCompleter* m_completer = nullptr;
    std::function<bool()> m_spaceSubmits;
    // When typing was forwarded from a view, the completion popup follows the
    // MOUSE CURSOR (suggestions where you look) instead of the bottom bar.
    QPoint m_popupAnchor;
    bool m_popupAnchored = false;
};

} // namespace viki
