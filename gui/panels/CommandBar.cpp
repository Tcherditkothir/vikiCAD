#include "CommandBar.h"

#include <QKeyEvent>
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
    m_history->setMaximumHeight(84);
    m_history->setFrameShape(QFrame::NoFrame);
    m_history->setFocusPolicy(Qt::NoFocus);
    QFont mono(QStringLiteral("DejaVu Sans Mono"));
    mono.setPointSize(9);
    m_history->setFont(mono);

    m_prompt = new QLabel(QStringLiteral("Command:"), this);
    m_prompt->setContentsMargins(6, 0, 4, 0);
    m_prompt->setStyleSheet(QStringLiteral("color:#4ba3ff;font-weight:600;"));

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(QStringLiteral("Type a command (LINE, CIRCLE, ARC, ...)"));
    m_input->setFont(mono);

    auto* inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(0, 0, 0, 0);
    inputRow->addWidget(m_prompt);
    inputRow->addWidget(m_input, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_history);
    layout->addLayout(inputRow);

    connect(m_input, &QLineEdit::returnPressed, this, [this] { submitLine(); });
    m_input->installEventFilter(this);
}

void CommandBar::submitLine()
{
    const QString line = m_input->text().trimmed();
    if (!line.isEmpty())
        appendHistory(QStringLiteral("> %1").arg(line));
    m_input->clear();
    emit commandEntered(line);
}

bool CommandBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space && (!m_spaceSubmits || m_spaceSubmits())) {
            submitLine(); // AutoCAD: Space confirms like Enter
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            m_input->clear();
            emit cancelRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CommandBar::beginTyping(const QString& seed)
{
    m_input->setFocus();
    m_input->insert(seed);
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
