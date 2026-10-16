#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

namespace muffin {
// Reconstruct SFNT from local WOFF/WOFF2 through FreeType's webfont decoder.
// The result is consumed by the native Qt font backend, including DirectWrite.
QByteArray decodeThemeWebFont(const QByteArray& source);
// Private in-memory SFNT identity. CSS descriptors define matching independently
// of a file's internal family/weight/style. Original files remain untouched.
QByteArray isolateThemeFont(const QByteArray& native, const QString& family, std::optional<int> weight = {},
                            std::optional<bool> italic = {});
// The selected candidate is decoded/validated and content-addressed once.
// Original bytes/aliases remain scoped to this load; only registration identities persist.
struct PreparedThemeFont {
  QByteArray native, digest;
};
struct ThemeFontRegistrationStats {
  qsizetype faces = 0, nativeBytesSubmitted = 0;
};
// Cumulative successful private registrations, not Qt/OS resident font memory.
ThemeFontRegistrationStats themeFontRegistrationStats();
PreparedThemeFont prepareThemeFont(const QByteArray& source, std::optional<int> weight = {},
                                   std::optional<bool> italic = {});
QString registerPrivateFont(const PreparedThemeFont& source, const QString& family, std::optional<int> weight = {},
                            std::optional<bool> italic = {});
QString registerPrivateFont(const QByteArray& source, const QString& family, std::optional<int> weight = {},
                            std::optional<bool> italic = {});
}  // namespace muffin
