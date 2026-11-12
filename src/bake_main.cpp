#include "teyvat/walk_surface.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <iostream>
#include <stdexcept>

namespace {
double number(const QJsonValue& value) {
    if (!value.isDouble()) { throw std::invalid_argument("Expected a numeric coordinate or setting"); }
    return value.toDouble();
}
}

int main(int argc, char** argv) try {
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() != 3) { throw std::invalid_argument("Usage: TeyvatBake source.json output.json"); }
    QFile input(arguments[1]);
    if (!input.open(QIODevice::ReadOnly) || input.size() > 128*1024*1024) { throw std::invalid_argument("Cannot open bounded geometry source"); }
    const auto bytes = input.readAll();
    QJsonParseError error;
    const auto root = QJsonDocument::fromJson(bytes, &error).object();
    if (error.error != QJsonParseError::NoError || root.value("format").toString() != QStringLiteral("teyvat-walk-surface-source-v1")) {
        throw std::invalid_argument("Unsupported geometry source");
    }
    const auto bounds = root.value("bounds").toArray();
    if (bounds.size() != 4) { throw std::invalid_argument("Expected min X, max X, min Z and max Z"); }
    teyvat::WalkBakeSettings settings;
    settings.min_x = number(bounds[0]); settings.max_x = number(bounds[1]);
    settings.min_z = number(bounds[2]); settings.max_z = number(bounds[3]);
    settings.scene = root.value("scene").toString().toStdString();
    settings.floor = root.value("floor").toString().toStdString();
    const auto options = root.value("settings").toObject();
    settings.spacing = number(options.value("spacing")); settings.radius = number(options.value("radius"));
    settings.height = number(options.value("height")); settings.step = number(options.value("step"));
    settings.max_slope_degrees = number(options.value("max_slope_degrees"));
    settings.metres_per_second = number(options.value("metres_per_second"));
    const auto rows = root.value("faces").toArray();
    if (rows.isEmpty() || rows.size() > 1000000) { throw std::invalid_argument("Geometry face count is outside the supported tile bounds"); }
    std::vector<teyvat::CollisionFace> faces;
    faces.reserve(rows.size());
    for (const auto row : rows) {
        const auto values = row.toArray();
        if (values.size() != 10 || !values[9].isBool()) { throw std::invalid_argument("Invalid collision face"); }
        faces.push_back({{{{number(values[0]), number(values[1]), number(values[2])},
            {number(values[3]), number(values[4]), number(values[5])},
            {number(values[6]), number(values[7]), number(values[8])}}}, values[9].toBool()});
    }
    const auto surface = teyvat::bake_walk_surface(faces, settings);
    QJsonArray nodes, passages;
    for (const auto& node : surface.nodes) { nodes.append(QJsonArray{node.x, node.y, node.z}); }
    for (const auto& passage : surface.passages) {
        passages.append(QJsonArray{static_cast<qint64>(passage.from), static_cast<qint64>(passage.to), *passage.seconds});
    }
    const QJsonObject output{{"format", QStringLiteral("teyvat-walk-surface-v1")},
        {"scene", root.value("scene")}, {"floor", root.value("floor")}, {"source", root.value("source")},
        {"source_sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
        {"bounds", bounds}, {"settings", options}, {"nodes", nodes}, {"passages", passages}};
    const auto result = QJsonDocument(output).toJson(QJsonDocument::Compact);
    QSaveFile file(arguments[2]);
    if (!file.open(QIODevice::WriteOnly) || file.write(result) != result.size() || !file.commit()) {
        throw std::runtime_error("Could not write the walk surface atomically");
    }
    std::cout << "Baked " << surface.nodes.size() << " standing positions and " << surface.passages.size() << " directed passages\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
