#pragma once

#include <QString>

#include "doc/Document.h"
#include "doc/Layout.h"

namespace viki {

// Plots a layout to PDF at exact scale: a 100 mm model line in a 1:1
// viewport measures 100 mm on the page. Non-printable layers (sticky
// notes by default) are excluded unless includeNotes.
bool plotToPdf(const Document& doc, const Layout& layout, const QString& path,
               QString& error, bool includeNotes = false);

} // namespace viki
