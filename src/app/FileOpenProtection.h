#pragma once

#include <QString>
#include <QStringList>

namespace muffin::file_open_protection {

inline constexpr auto kExtensionEnabled = "files/extensionProtection";
inline constexpr auto kTextExtensions = "files/textExtensions";
inline constexpr auto kSizeEnabled = "files/sizeProtection";
inline constexpr auto kSizeLimitMb = "files/sizeLimitMb";
inline constexpr auto kSizeLimitBytes = "files/sizeLimitBytes";
inline constexpr auto kSizeLimitUnit = "files/sizeLimitUnit";
inline constexpr int kDefaultSizeLimitMb = 100;
inline constexpr int kMaximumSizeLimitMb = 1024 * 1024;
inline constexpr qint64 kMinimumSizeLimitBytes = 1024;
inline constexpr qint64 kMaximumSizeLimitBytes = qint64(kMaximumSizeLimitMb) * 1024 * 1024;

QString defaultTextExtensions();
QStringList normalizedTextExtensions(const QString& text);
qint64 sizeLimitBytes();

struct Warnings {
  bool extension = false;
  bool size = false;
  bool any() const { return extension || size; }
};

// Inspect metadata only: confirmation must precede reading or decoding the file.
Warnings warningsForFile(const QString& path);

}  // namespace muffin::file_open_protection
