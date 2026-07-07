#include "CommandBar.h"

#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace viki {

CommandBar::CommandBar(QWidget* parent)
    : QWidget(parent)
{
    m_history = new QPlainTextEdit(this);
    m_history->setReadOnly(true);
    m_history->setMaximumHeight(80);
    m_history->setFrameShape(QFrame::NoFrame);
    m_history->setFocusPolicy(Qt::NoFocus);

    m_prompt = new QLabel(QStringLiteral("Command:"), this);
    m_prompt->setContentsMargins(4, 0, 4, 0);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(QStringLiteral("Type a command (LINE, CIRCLE, ARC, ...)"));

    auto* inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(0, 0, 0, 0);
    inputRow->addWidget(m_prompt);
    inputRow->addWidget(m_input, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_history);
    layout->addLayout(inputRow);

    connect(m_input, &QLineEdit::returnPressed, this, [this] {
        const QString line = m_input->text().trimmed();
        if (!line.isEmpty())
            appendHistory(QStringLiteral("> %1").arg(line));
        m_input->clear();
        emit commandEntered(line);
    });
}

void CommandBar::appendHistory(const QString& text)
{
    m_history->appendPlainText(text);
}

void CommandBar::setPrompt(const QString& prompt)
{
    m_prompt->setText(prompt.isEmpty() ? QStringLiteral("Command:") : prompt);
}

void CommandBar::focusInput()
{
    m_input->setFocus();
}

} // namespace viki
