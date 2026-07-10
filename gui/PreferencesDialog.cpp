#include "PreferencesDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

namespace viki {

namespace {
int indexForButton(const QString& name)
{
    if (name.compare(QLatin1String("middle"), Qt::CaseInsensitive) == 0)
        return 1;
    if (name.compare(QLatin1String("right"), Qt::CaseInsensitive) == 0)
        return 2;
    return 0; // left
}
QString buttonForIndex(int index)
{
    switch (index) {
    case 1: return QStringLiteral("middle");
    case 2: return QStringLiteral("right");
    default: return QStringLiteral("left");
    }
}
} // namespace

PreferencesDialog::PreferencesDialog(const MousePrefs& mouse,
                                     const QJsonObject& shortcuts,
                                     QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Preferences"));
    auto* layout = new QVBoxLayout(this);

    // --- Mouse (3D view) ---
    auto* mouseBox = new QGroupBox(QStringLiteral("Mouse — 3D view"), this);
    auto* form = new QFormLayout(mouseBox);
    const QStringList buttons{QStringLiteral("Left button"),
                              QStringLiteral("Middle button"),
                              QStringLiteral("Right button")};
    m_orbit = new QComboBox(mouseBox);
    m_orbit->addItems(buttons);
    m_orbit->setCurrentIndex(indexForButton(mouse.orbit));
    form->addRow(QStringLiteral("Orbit (rotate):"), m_orbit);
    m_pan = new QComboBox(mouseBox);
    m_pan->addItems(buttons);
    m_pan->setCurrentIndex(indexForButton(mouse.pan));
    form->addRow(QStringLiteral("Pan:"), m_pan);
    m_zoomInvert = new QCheckBox(QStringLiteral("Invert zoom wheel"), mouseBox);
    m_zoomInvert->setChecked(mouse.zoomInvert);
    form->addRow(QString(), m_zoomInvert);
    layout->addWidget(mouseBox);

    // --- Keyboard shortcuts ---
    auto* keysBox = new QGroupBox(QStringLiteral("Keyboard shortcuts"), this);
    auto* keysLayout = new QVBoxLayout(keysBox);
    keysLayout->addWidget(new QLabel(
        QStringLiteral("Each row maps a key sequence to a command line "
                       "(e.g. Ctrl+Z → UNDO). Empty rows are ignored."),
        keysBox));
    m_shortcutTable = new QTableWidget(keysBox);
    m_shortcutTable->setColumnCount(2);
    m_shortcutTable->setHorizontalHeaderLabels(
        {QStringLiteral("Key"), QStringLiteral("Command")});
    m_shortcutTable->horizontalHeader()->setStretchLastSection(true);
    m_shortcutTable->verticalHeader()->setVisible(false);
    m_shortcutTable->setRowCount(shortcuts.size() + 4); // room to add new ones
    int row = 0;
    for (auto it = shortcuts.begin(); it != shortcuts.end(); ++it, ++row) {
        m_shortcutTable->setItem(row, 0, new QTableWidgetItem(it.key()));
        m_shortcutTable->setItem(row, 1,
                                 new QTableWidgetItem(it.value().toString()));
    }
    keysLayout->addWidget(m_shortcutTable);
    layout->addWidget(keysBox, /*stretch=*/1);

    auto* buttonsBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonsBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonsBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttonsBox);
    resize(460, 480);
}

MousePrefs PreferencesDialog::mousePrefs() const
{
    MousePrefs p;
    p.orbit = buttonForIndex(m_orbit->currentIndex());
    p.pan = buttonForIndex(m_pan->currentIndex());
    if (p.pan == p.orbit) // never the same button for both
        p.pan = p.orbit == QLatin1String("middle") ? QStringLiteral("right")
                                                   : QStringLiteral("middle");
    p.zoomInvert = m_zoomInvert->isChecked();
    return p;
}

QJsonObject PreferencesDialog::shortcuts() const
{
    QJsonObject map;
    for (int row = 0; row < m_shortcutTable->rowCount(); ++row) {
        const auto* keyItem = m_shortcutTable->item(row, 0);
        const auto* cmdItem = m_shortcutTable->item(row, 1);
        if (!keyItem || !cmdItem)
            continue;
        const QString key = keyItem->text().trimmed();
        const QString cmd = cmdItem->text().trimmed();
        if (key.isEmpty() || cmd.isEmpty())
            continue;
        if (QKeySequence(key).isEmpty())
            continue; // not a parseable key sequence
        map[QKeySequence(key).toString()] = cmd.toUpper();
    }
    return map;
}

} // namespace viki
