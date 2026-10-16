#include "theme/ThemeResourceDigest.h"

#include <QCryptographicHash>
#include <QFile>
#include <optional>

#include "diagnostics/ScopedPerfProbe.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>

#include <bcrypt.h>
#endif

namespace muffin {
namespace {
Q_LOGGING_CATEGORY(resourceDigestPerf, "muffin.perf.detail", QtWarningMsg)

template <typename Append>
bool streamResource(QFile& file, const Append& append) {
  QByteArray buffer(256 * 1024, Qt::Uninitialized);
  for (;;) {
    const auto size = [&] {
      diag::ScopedPerfProbe perf("theme.digestRead", resourceDigestPerf());
      return file.read(buffer.data(), buffer.size());
    }();
    if (size < 0) return false;
    if (!size) return true;
    diag::ScopedPerfProbe perf("theme.digestHash", resourceDigestPerf());
    if (!append(QByteArrayView(buffer.constData(), size))) return false;
  }
}

#ifdef Q_OS_WIN
struct Sha256Provider {
  Sha256Provider() {
    if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) handle = nullptr;
  }
  ~Sha256Provider() {
    if (handle) BCryptCloseAlgorithmProvider(handle, 0);
  }
  BCRYPT_ALG_HANDLE handle = nullptr;
};

struct NativeHash {
  ~NativeHash() {
    if (handle) BCryptDestroyHash(handle);
  }
  BCRYPT_HASH_HANDLE handle = nullptr;
};

const Sha256Provider& sha256Provider() {
  static const Sha256Provider provider;
  return provider;
}

template <typename Feed>
std::optional<QByteArray> nativeDigest(const Feed& feed) {
  NativeHash hash;
  {
    diag::ScopedPerfProbe perf("theme.digestNativeSetup", resourceDigestPerf());
    // Algorithm handles can be shared; each verification owns its hash state.
    // CNG manages the hash-object storage (supported since Windows 7).
    const auto& provider = sha256Provider();
    if (!provider.handle || BCryptCreateHash(provider.handle, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0)
      return std::nullopt;
  }
  if (!feed([&](QByteArrayView bytes) {
        // Bound each CNG call independently of the size of an in-memory view.
        // Views can exceed ULONG; slicing never allocates or copies input.
        while (!bytes.isEmpty()) {
          const auto size = qMin<qsizetype>(bytes.size(), 256 * 1024);
          if (BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                             static_cast<ULONG>(size), 0) < 0)
            return false;
          bytes = bytes.sliced(size);
        }
        return true;
      }))
    return std::nullopt;
  QByteArray digest(32, Qt::Uninitialized);
  if (BCryptFinishHash(hash.handle, reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()), 0) < 0)
    return std::nullopt;
  return digest;
}
#endif
}  // namespace

QByteArray themeResourceDigest(const QString& path) {
  QFile file(path);
  {
    diag::ScopedPerfProbe perf("theme.digestOpen", resourceDigestPerf());
    if (!file.open(QIODevice::ReadOnly)) return {};
  }
#ifdef Q_OS_WIN
  if (const auto digest = nativeDigest([&](const auto& append) { return streamResource(file, append); }))
    return *digest;
  // A provider/read failure must never reuse a partial digest. Start a full Qt
  // verification from the beginning; a persistent I/O error remains unreadable.
  if (!file.seek(0)) return {};
  diag::ScopedPerfProbe fallbackPerf("theme.digestFallback", resourceDigestPerf());
#endif
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!streamResource(file, [&](QByteArrayView bytes) {
        hash.addData(bytes);
        return true;
      }))
    return {};
  return hash.result();
}

QByteArray themeContentDigest(QByteArrayView bytes) {
  diag::ScopedPerfProbe perf("theme.memoryDigest", resourceDigestPerf());
#ifdef Q_OS_WIN
  if (const auto digest = nativeDigest([&](const auto& append) { return append(bytes); })) return *digest;
  diag::ScopedPerfProbe fallbackPerf("theme.memoryDigestFallback", resourceDigestPerf());
#endif
  // A native failure discards its partial hash; Qt starts from the full view.
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

}  // namespace muffin
