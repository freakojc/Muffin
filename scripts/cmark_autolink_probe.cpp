#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <cstdio>

#include "../tests/parser/CmarkAutolinkSnapshot.h"

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc != 2)
    return 2;
  QFile file(QString::fromLocal8Bit(argv[1]));
  if (!file.open(QIODevice::ReadOnly))
    return 3;
  const auto inputs = QJsonDocument::fromJson(file.readAll()).array();
  const auto records = autolink_test::run(inputs);
  std::printf("%s\n", QJsonDocument(records).toJson(QJsonDocument::Compact).constData());
}
