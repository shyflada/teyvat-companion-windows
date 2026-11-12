#include "teyvat/evaluation.hpp"
#include "teyvat/version.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <cstdio>
#include <stdexcept>

namespace {
QByteArray read(const QString& path, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > limit) { throw std::runtime_error("Cannot read bounded dataset/labels file."); }
    const auto bytes = file.read(limit + 1); if (bytes.size() > limit || file.error() != QFile::NoError) { throw std::runtime_error("Input read failed."); } return bytes;
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); app.setApplicationName(QStringLiteral("TeyvatEvaluate")); app.setApplicationVersion(QString::fromLatin1(teyvat::application_version));
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("Read-only offline recording evaluation; no database, capture or network."));
    parser.addHelpOption(); parser.addVersionOption();
    for (const auto& option : {QStringLiteral("recording"), QStringLiteral("dataset"), QStringLiteral("labels"), QStringLiteral("output")}) {
        parser.addOption({option, QStringLiteral("Path for %1.").arg(option), QStringLiteral("path")});
    }
    parser.process(app);
    try {
        for (const auto& option : {QStringLiteral("recording"), QStringLiteral("dataset"), QStringLiteral("labels"), QStringLiteral("output")}) {
            if (!parser.isSet(option) || parser.value(option).isEmpty()) { throw std::runtime_error("Provide --recording, --dataset, --labels and a new --output filename."); }
        }
        if (QFileInfo::exists(parser.value(QStringLiteral("output")))) { throw std::runtime_error("Choose a new output filename; existing files are preserved."); }
        const auto dataset = teyvat::decode_json(read(parser.value(QStringLiteral("dataset")), teyvat::max_transfer_bytes));
        const auto labels = read(parser.value(QStringLiteral("labels")), 1024 * 1024);
        const auto analysis = teyvat::analyze_recording(parser.value(QStringLiteral("recording")));
        const auto report = teyvat::evaluate_recording(analysis, dataset, labels);
        teyvat::atomic_export(parser.value(QStringLiteral("output")), QJsonDocument(report).toJson());
        std::puts("Evaluation report saved. No progress or input files were changed."); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "TeyvatEvaluate: %s\n", error.what()); return 1; }
}
