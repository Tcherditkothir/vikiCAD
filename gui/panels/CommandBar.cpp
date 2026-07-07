#include "CommandBar.h"

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

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(QStringLiteral("Type a command"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_history);
    layout->addWidget(m_input);

    connect(m_input, &QLineEdit::returnPressed, this, [this] {
        const QString line = m_input->text().trimmed();
        if (line.isEmpty())
            return;
        appendHistory(QStringLiteral("> %1").arg(line));
        m_input->clear();
        emit commandEntered(line);
    });
}

void CommandBar::appendHistory(const QString& text)
{
    m_history->appendPlainText(text);
}

} // namespace viki
