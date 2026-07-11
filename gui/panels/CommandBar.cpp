#include "CommandBar.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QStringListModel>
#include <QTimer>
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
            // Qt re-anchors the popup to the line edit on every keystroke —
            // pull it back to the mouse-cursor anchor once Qt is done.
            QTimer::singleShot(0, this,
                               [this] { moveCompleterPopupToAnchor(); });
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
    m_popupAnchored = false; // the typing session is over
    const QString line = m_input->text().trimmed();
    if (!line.isEmpty())
        appendHistory(QStringLiteral("> %1").arg(line));
    m_input->clear();
    emit commandEntered(line);
}

bool CommandBar::eventFilter(QObject* watched, QEvent* event)
{
    // Keys over the completion POPUP: QCompleter forwards unhandled keys
    // STRAIGHT to the line edit's internal handler, bypassing the m_input
    // event filter below — so while the popup was visible, SPACE typed a
    // literal space instead of submitting (Lex's "weird" find), and Escape
    // only closed the popup. Handle both here.
    if (m_completer && watched == m_completer->popup() &&
        event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            m_popupAnchored = false;
            m_completer->popup()->hide();
            m_input->clear();
            emit cancelRequested();
            return true;
        }
        if (key->key() == Qt::Key_Space &&
            (!m_spaceSubmits || m_spaceSubmits())) {
            m_completer->popup()->hide();
            submitLine(); // AutoCAD: Space confirms like Enter
            return true;
        }
    }
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space && (!m_spaceSubmits || m_spaceSubmits())) {
            submitLine(); // AutoCAD: Space confirms like Enter
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            m_popupAnchored = false;
            m_input->clear();
            emit cancelRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CommandBar::beginTyping(const QString& seed)
{
    // Typing forwarded from a view: anchor the suggestion popup AT THE MOUSE
    // CURSOR — suggestions appear where the user is looking, not down in the
    // bar ("le menu au curseur").
    m_popupAnchor = QCursor::pos() + QPoint(14, 18);
    m_popupAnchored = true;
    m_input->setFocus();
    m_input->insert(seed);
    // insert() bypasses QLineEdit's key handling, so the completion popup
    // never appeared when typing OVER the canvas/3D view (the keystrokes are
    // forwarded here). Open it explicitly — with no auto-highlighted row.
    if (m_completer && !m_input->text().isEmpty()) {
        m_completer->setCompletionPrefix(m_input->text());
        m_completer->complete();
        m_completer->popup()->setCurrentIndex(QModelIndex());
        moveCompleterPopupToAnchor();
    }
}

void CommandBar::moveCompleterPopupToAnchor()
{
    if (!m_popupAnchored || !m_completer || !m_completer->popup() ||
        !m_completer->popup()->isVisible())
        return;
    m_completer->popup()->move(m_popupAnchor);
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
    m_popupAnchored = false; // deliberate focus: suggestions at the bar
    m_input->setFocus();
}

} // namespace viki
