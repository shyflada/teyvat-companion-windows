#include "teyvat/map_data.hpp"
#include "sql.hpp"
#include <QBuffer>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QStringConverter>
#include <cmath>

namespace teyvat {
namespace {
using sql::fail;

void check_text(const QString& value, int limit, bool required = true, bool multiline = false) {
    if ((required && value.trimmed().isEmpty()) || value.size() > limit) { fail(QStringLiteral("Missing or oversized text field.")); }
    for (qsizetype i = 0; i < value.size(); ++i) {
        const auto c = value.at(i);
        if (c.category() == QChar::Other_Control && !(multiline && (c == '\n' || c == '\r' || c == '\t'))) {
            fail(QStringLiteral("Control characters are not allowed in identifiers or labels."));
        }
        if (c.isHighSurrogate()) {
            if (++i >= value.size() || !value.at(i).isLowSurrogate()) { fail(QStringLiteral("Invalid Unicode text.")); }
        } else if (c.isLowSurrogate()) { fail(QStringLiteral("Invalid Unicode text.")); }
    }
}

QString utf8(const QByteArray& data) {
    if (data.size() > max_transfer_bytes) { fail(QStringLiteral("Import exceeds the 8 MiB limit.")); }
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString result = decoder(data);
    if (decoder.hasError()) { fail(QStringLiteral("Import is not valid UTF-8.")); }
    if (result.startsWith(QChar(0xfeff))) { result.remove(0, 1); }
    return result;
}

QString field(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString()) { fail(QStringLiteral("Expected text field: %1").arg(QLatin1String(key))); }
    return value.toString();
}

double number(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isDouble() || !std::isfinite(value.toDouble())) { fail(QStringLiteral("Expected finite numeric field: %1").arg(QLatin1String(key))); }
    return value.toDouble();
}

QJsonObject layer_json(const MapLayer& layer) {
    return {{"source", layer.key.source}, {"region", layer.key.region}, {"layer", layer.key.layer},
            {"title", layer.title}, {"frame", layer.frame}, {"width", layer.width}, {"height", layer.height},
            {"provenance", layer.provenance}, {"license", layer.license}, {"data_version", layer.version}};
}

QJsonObject marker_json(const MapMarker& marker) {
    return {{"source", marker.map.source}, {"region", marker.map.region}, {"layer", marker.map.layer},
            {"id", marker.id}, {"category", marker.category}, {"label", marker.label}, {"notes", marker.notes},
            {"x", marker.x}, {"y", marker.y}, {"status", stored_status(marker.status)},
            {"retired", marker.retired}, {"origin", marker.origin}};
}

const QStringList csv_columns{QStringLiteral("format_version"), QStringLiteral("record_type"),
    QStringLiteral("source"), QStringLiteral("region"), QStringLiteral("layer"), QStringLiteral("title"),
    QStringLiteral("frame"), QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("provenance"),
    QStringLiteral("license"), QStringLiteral("data_version"), QStringLiteral("id"), QStringLiteral("category"),
    QStringLiteral("label"), QStringLiteral("notes"), QStringLiteral("x"), QStringLiteral("y"),
    QStringLiteral("status"), QStringLiteral("retired"), QStringLiteral("origin")};

QString csv_cell(QString value) {
    // The v1 interchange contract prefixes EVERY data cell. It preserves leading
    // apostrophes/zeroes and prevents formula-leading text becoming executable.
    value.prepend(QChar('\''));
    value.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QVector<QStringList> csv_rows(const QString& text) {
    QVector<QStringList> rows;
    QStringList row;
    QString cell;
    bool quoted = false, closed = false, start = true;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text.at(i + 1) == '"') { cell += '"'; ++i; }
                else { quoted = false; closed = true; }
            } else { cell += c; }
        } else if (c == '"') {
            if (!start) { fail(QStringLiteral("Unexpected quote in CSV.")); }
            quoted = true; start = false;
        } else if (c == ',' || c == '\r' || c == '\n') {
            row.append(cell); cell.clear(); closed = false; start = true;
            if (row.size() > csv_columns.size()) { fail(QStringLiteral("Too many CSV columns.")); }
            if (c != ',') {
                if (c == '\r' && i + 1 < text.size() && text.at(i + 1) == '\n') { ++i; }
                rows.append(row); row.clear();
                if (rows.size() > max_layer_markers + max_map_layers + 1) { fail(QStringLiteral("Too many CSV records.")); }
            }
        } else {
            if (closed) { fail(QStringLiteral("Unexpected text after a quoted CSV cell.")); }
            cell += c; start = false;
        }
    }
    if (quoted) { fail(QStringLiteral("Unclosed CSV quote.")); }
    if (!cell.isEmpty() || !row.isEmpty() || !start) { row.append(cell); rows.append(row); }
    return rows;
}
} // namespace

QString layer_identity(const LayerKey& key) { return key.source + QChar(31) + key.region + QChar(31) + key.layer; }
QString marker_identity(const MapMarker& marker) { return marker.map.source + QChar(31) + marker.id; }
QString stored_status(CollectionStatus status) { return QString::fromLatin1(status_name(status).data(), static_cast<qsizetype>(status_name(status).size())); }
CollectionStatus read_status(const QString& status) {
    const auto value = parse_status(status.toStdString());
    if (!value) { sql::fail(QStringLiteral("Unrecognized collection status.")); }
    return *value;
}

void validate_dataset(const Dataset& dataset) {
    if (dataset.maps.size() > max_map_layers || dataset.markers.size() > max_layer_markers) { sql::fail(QStringLiteral("A transfer supports at most 1024 layers and 5000 markers.")); }
    QHash<QString, MapLayer> maps;
    for (const auto& map : dataset.maps) {
        for (const auto& text : {map.key.source, map.key.region, map.key.layer, map.frame, map.version}) { check_text(text, 128); }
        check_text(map.title, 120); check_text(map.provenance, 512); check_text(map.license, 512);
        if (!std::isfinite(map.width) || !std::isfinite(map.height) || map.width <= 0 || map.height <= 0
            || map.width > 1e6 || map.height > 1e6) { sql::fail(QStringLiteral("Map dimensions must be between 0 and 1,000,000 units.")); }
        const auto id = layer_identity(map.key);
        if (maps.contains(id)) { sql::fail(QStringLiteral("Duplicate map layer.")); }
        maps.insert(id, map);
    }
    QSet<QString> keys;
    for (const auto& marker : dataset.markers) {
        check_text(marker.id, 128); check_text(marker.category, 64); check_text(marker.label, 120);
        check_text(marker.notes, 2000, false, true);
        (void)stored_status(marker.status);
        if (marker.origin != QStringLiteral("manual") && marker.origin != QStringLiteral("import")) { sql::fail(QStringLiteral("Invalid marker origin.")); }
        const auto map = maps.constFind(layer_identity(marker.map));
        if (map == maps.cend()) { sql::fail(QStringLiteral("Every marker must reference a layer included in the transfer.")); }
        if (!std::isfinite(marker.x) || !std::isfinite(marker.y) || marker.x < 0 || marker.y < 0
            || marker.x > map->width || marker.y > map->height) { sql::fail(QStringLiteral("Marker is outside its map bounds.")); }
        const auto key = marker_identity(marker);
        if (keys.contains(key)) { sql::fail(QStringLiteral("Duplicate source/marker identity.")); }
        keys.insert(key);
    }
}

QByteArray encode_json(const Dataset& dataset) {
    validate_dataset(dataset);
    QJsonArray maps, markers;
    for (const auto& map : dataset.maps) { maps.append(layer_json(map)); }
    for (const auto& marker : dataset.markers) { markers.append(marker_json(marker)); }
    const auto bytes = QJsonDocument(QJsonObject{{"format", "teyvat-companion"}, {"version", 1},
                                               {"maps", maps}, {"markers", markers}}).toJson(QJsonDocument::Compact);
    if (bytes.size() > max_transfer_bytes) { sql::fail(QStringLiteral("Export exceeds the 8 MiB interchange limit. Use a full database backup for this layer.")); }
    return bytes;
}

Dataset decode_json(const QByteArray& bytes) {
    const auto text = utf8(bytes);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) { sql::fail(QStringLiteral("Invalid JSON document.")); }
    const auto root = document.object();
    if (root.value(QStringLiteral("format")) != QJsonValue(QStringLiteral("teyvat-companion"))
        || root.value(QStringLiteral("version")) != QJsonValue(1)
        || !root.value(QStringLiteral("maps")).isArray() || !root.value(QStringLiteral("markers")).isArray()) { sql::fail(QStringLiteral("Unsupported interchange format/version.")); }
    Dataset result;
    for (const auto& value : root.value(QStringLiteral("maps")).toArray()) {
        if (!value.isObject()) { sql::fail(QStringLiteral("Invalid layer record.")); }
        const auto map = value.toObject();
        result.maps.append({{field(map, "source"), field(map, "region"), field(map, "layer")},
            field(map, "title"), field(map, "frame"), number(map, "width"), number(map, "height"),
            field(map, "provenance"), field(map, "license"), field(map, "data_version")});
    }
    for (const auto& value : root.value(QStringLiteral("markers")).toArray()) {
        if (!value.isObject()) { sql::fail(QStringLiteral("Invalid marker record.")); }
        const auto marker = value.toObject();
        if (!marker.value(QStringLiteral("retired")).isBool()) { sql::fail(QStringLiteral("Expected boolean retired field.")); }
        result.markers.append({{field(marker, "source"), field(marker, "region"), field(marker, "layer")},
            field(marker, "id"), field(marker, "category"), field(marker, "label"), field(marker, "notes"),
            number(marker, "x"), number(marker, "y"), read_status(field(marker, "status")),
            marker.value(QStringLiteral("retired")).toBool(), field(marker, "origin")});
    }
    validate_dataset(result);
    return result;
}

QByteArray encode_csv(const Dataset& dataset) {
    validate_dataset(dataset);
    QString result = csv_columns.join(',') + QStringLiteral("\r\n");
    const auto append = [&](const QJsonObject& object, const QString& type) {
        QStringList cells;
        for (const auto& key : csv_columns) {
            QString value;
            if (key == QStringLiteral("format_version")) { value = QStringLiteral("1"); }
            else if (key == QStringLiteral("record_type")) { value = type; }
            else if (object.contains(key)) {
                const auto field_value = object.value(key);
                value = field_value.isDouble() ? QString::number(field_value.toDouble(), 'g', 17)
                    : field_value.isBool() ? (field_value.toBool() ? QStringLiteral("true") : QStringLiteral("false")) : field_value.toString();
            }
            cells.append(csv_cell(value));
        }
        result += cells.join(',') + QStringLiteral("\r\n");
    };
    for (const auto& layer : dataset.maps) { append(layer_json(layer), QStringLiteral("layer")); }
    for (const auto& marker : dataset.markers) { append(marker_json(marker), QStringLiteral("marker")); }
    const auto bytes = result.toUtf8();
    if (bytes.size() > max_transfer_bytes) { sql::fail(QStringLiteral("CSV export exceeds 8 MiB.")); }
    return bytes;
}

Dataset decode_csv(const QByteArray& bytes) {
    auto rows = csv_rows(utf8(bytes));
    if (rows.isEmpty() || rows.takeFirst() != csv_columns) { sql::fail(QStringLiteral("Unexpected CSV header.")); }
    QJsonArray maps, markers;
    for (auto& row : rows) {
        if (row.size() != csv_columns.size()) { sql::fail(QStringLiteral("Incorrect CSV column count.")); }
        QJsonObject object;
        for (qsizetype i = 0; i < row.size(); ++i) {
            if (!row[i].startsWith('\'')) { sql::fail(QStringLiteral("CSV v1 data cells must use apostrophe-v1 encoding.")); }
            row[i].remove(0, 1);
            object.insert(csv_columns[i], row[i]);
        }
        if (field(object, "format_version") != QStringLiteral("1")) { sql::fail(QStringLiteral("Unsupported CSV version.")); }
        const auto type = field(object, "record_type");
        const QStringList numeric = type == QStringLiteral("layer") ? QStringList{QStringLiteral("width"), QStringLiteral("height")}
            : QStringList{QStringLiteral("x"), QStringLiteral("y")};
        for (const auto& key : numeric) {
            bool ok = false;
            const double value = object.value(key).toString().toDouble(&ok);
            if (!ok || !std::isfinite(value)) { sql::fail(QStringLiteral("Invalid CSV number.")); }
            object.insert(key, value);
        }
        if (type == QStringLiteral("layer")) { maps.append(object); }
        else if (type == QStringLiteral("marker")) {
            const auto retired = field(object, "retired");
            if (retired != QStringLiteral("true") && retired != QStringLiteral("false")) { sql::fail(QStringLiteral("Invalid CSV retired flag.")); }
            object.insert(QStringLiteral("retired"), retired == QStringLiteral("true")); markers.append(object);
        } else { sql::fail(QStringLiteral("Unrecognized CSV record type.")); }
    }
    return decode_json(QJsonDocument(QJsonObject{{"format", "teyvat-companion"}, {"version", 1},
                                                {"maps", maps}, {"markers", markers}}).toJson(QJsonDocument::Compact));
}

QImage decode_background(const QByteArray& png) {
    if (png.isEmpty() || png.size() > 16 * 1024 * 1024) { sql::fail(QStringLiteral("PNG must be nonempty and no larger than 16 MiB.")); }
    QBuffer buffer; buffer.setData(png); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png"); reader.setAutoDetectImageFormat(false);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 4096 || size.height() > 4096) { sql::fail(QStringLiteral("PNG dimensions must not exceed 4096 by 4096.")); }
    const auto image = reader.read();
    if (image.isNull()) { sql::fail(QStringLiteral("Cannot decode the PNG image.")); }
    return image;
}

void atomic_export(const QString& path, const QByteArray& data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) { sql::fail(QStringLiteral("Export could not be saved. The previous file was preserved.")); }
}
} // namespace teyvat
