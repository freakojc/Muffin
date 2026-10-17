#pragma once

#include <QString>

namespace muffin::file_open_protection {

inline constexpr auto kExtensionEnabled = "files/extensionProtection";
inline constexpr auto kTextExtensions = "files/textExtensions";
inline constexpr auto kSizeEnabled = "files/sizeProtection";
inline constexpr auto kSizeLimitMb = "files/sizeLimitMb";
inline constexpr int kDefaultSizeLimitMb = 100;
inline constexpr int kMaximumSizeLimitMb = 1024 * 1024;

QString defaultTextExtensions();
int sizeLimitMb();

struct Warnings {
  bool extension = false;
  bool size = false;
  bool any() const { return extension || size; }
};

// Inspect metadata only: confirmation must precede reading or decoding the file.
Warnings warningsForFile(const QString& path);

}  // namespace muffin::file_open_protection
