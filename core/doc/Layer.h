#pragma once

#include <cstdint>

#include <QString>

namespace viki {

using LayerId = int64_t;

struct Layer {
    LayerId id = 0;
    QString name = QStringLiteral("0");
    uint32_t rgb = 0xFFFFFF;
    bool visible = true;
    bool locked = false;
    bool printable = true;
};

} // namespace viki
