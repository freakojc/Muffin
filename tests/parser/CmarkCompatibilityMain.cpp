#include <QCoreApplication>

#include "../TestUtils.h"

void testCmarkFootnoteCompatibility(const QString& oraclePath);
void testCmarkAutolinkCompatibility(const QString& oraclePath);

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  require(argc == 2, "Missing cmark compatibility oracle path");
  const auto path = QString::fromLocal8Bit(argv[1]);
  testCmarkFootnoteCompatibility(path);
  testCmarkAutolinkCompatibility(path);
  return 0;
}
