#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include "Export.h"

namespace muffin {

// SHA-256 of the complete current contents, with bounded streaming storage.
// Empty means absent/unreadable; a readable empty file has its usual 32-byte hash.
// No persistent content or metadata cache. Windows uses CNG, with Qt fallback.
MUFFIN_CORE_EXPORT QByteArray themeResourceDigest(const QString& path);

// The same SHA-256 backend for already decoded font bytes. Borrows the view
// only for this call; does not copy or retain the complete input buffer.
MUFFIN_CORE_EXPORT QByteArray themeContentDigest(QByteArrayView bytes);

} // namespace muffin
