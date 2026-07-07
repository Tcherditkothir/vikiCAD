#pragma once

#include <memory>

#include <QString>

#include "doc/Document.h"

namespace viki {

// SQLite-based native format (.vkd). One call saves/loads the whole document
// in a single SQLite transaction; atomicity and crash-safety come from SQLite.
class NativeStore {
public:
    // Overwrites the document tables in `path` (creating the file if needed).
    // Returns false and sets `error` on failure.
    static bool save(const Document& doc, const QString& path, QString& error);

    // Loads `path` into a fresh Document; nullptr and `error` on failure.
    static std::unique_ptr<Document> load(const QString& path, QString& error);

    static constexpr int kSchemaVersion = 1;
    static constexpr int32_t kApplicationId = 0x56494B44; // 'VIKD'
};

} // namespace viki
