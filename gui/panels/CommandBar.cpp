#include "CommandBar.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QStringListModel>
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

void CommandBar::setCompletions(const QStringList& names)
{
    if (!m_completer) {
        m_completer = new QCompleter(this);
        m_completer->setCaseSensitivity(Qt::CaseInsensitive);
        m_completer->setCompletionMode(QCompleter::PopupCompletion);
        // MatchContains so "3D" surfaces MOVE3D/ROTATE3D/FILLET3D, etc.
        m_completer->setFilterMode(Qt::MatchContains);
        m_completer->setModel(new QStringListModel(names, m_completer));
        m_input->setCompleter(m_completer);
        // The popup must NEVER hijack Enter for a row the user did not pick:
        // Qt auto-highlights the first match, so typing "l" + Enter inserted
        // whatever command happened to contain an L instead of running LINE
        // ("je lançais des commandes au hasard"). Clear the auto-highlight on
        // every prefix change — Enter then submits the TYPED text (aliases
        // like L work); the arrow keys still pick a suggestion explicitly.
        connect(m_input, &QLineEdit::textChanged, this, [this] {
            if (m_completer && m_completer->popup())
                m_completer->popup()->setCurrentIndex(QModelIndex());
        });
        // And Escape over the open popup = cancel EVERYTHING in one press.
        m_completer->popup()->installEventFilter(this);
    } else {
        if (auto* model = qobject_cast<QStringListModel*>(m_completer->model()))
            model->setStringList(names);
    }
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
    // Escape over the completion POPUP: Qt would only close the popup and the
    // user had to press Escape AGAIN to actually cancel — one press now
    // closes the popup, clears the line AND cancels the running command.
    if (m_completer && watched == m_completer->popup() &&
        event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        m_completer->popup()->hide();
        m_input->clear();
        emit cancelRequested();
        return true;
    }
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
    // insert() bypasses QLineEdit's key handling, so the completion popup
    // never appeared when typing OVER the canvas/3D view (the keystrokes are
    // forwarded here). Open it explicitly — with no auto-highlighted row.
    if (m_completer && !m_input->text().isEmpty()) {
        m_completer->setCompletionPrefix(m_input->text());
        m_completer->complete();
        m_completer->popup()->setCurrentIndex(QModelIndex());
    }
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
