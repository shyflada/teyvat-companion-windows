#pragma once

#include "teyvat/map_data.hpp"
#include "teyvat/detection.hpp"

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

namespace teyvat {

struct Profile {
    QString id;
    QString name;
    QString created_at;
};

struct Settings {
    QString language = QStringLiteral("en");
    QString theme = QStringLiteral("dark");
    int ui_scale = 100;
    bool operator==(const Settings&) const = default;
};

// One connection owned by the constructing thread. No account or browser data.
class Store final {
public:
    static constexpr int schema_version = 6;
    static constexpr int application_id = 0x54435654;

    explicit Store(QString path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    [[nodiscard]] QString path() const;
    [[nodiscard]] QVector<Profile> profiles() const;
    [[nodiscard]] Profile active_profile() const;
    [[nodiscard]] QString create_profile(const QString& name);
    void activate_profile(const QString& id);
    void rename_profile(const QString& id, const QString& name);
    [[nodiscard]] Settings settings(const QString& profile_id) const;
    void save_settings(const QString& profile_id, const Settings& settings);
    [[nodiscard]] QByteArray window_geometry() const;
    void save_window_geometry(const QByteArray& geometry);

    [[nodiscard]] QVector<MapLayer> layers() const;
    [[nodiscard]] QVector<MapMarker> markers(const QString& profile, const LayerKey& layer) const;
    [[nodiscard]] Dataset export_layer(const QString& profile, const LayerKey& layer) const;
    [[nodiscard]] ImportReview review_import(const Dataset& data, const QString& profile, bool overwrite_progress) const;
    void apply_import(const Dataset& data, const QString& profile, bool overwrite_progress, const QByteArray& token);
    void save_layer(const MapLayer& layer);
    void save_marker(const QString& profile, const MapMarker& marker);
    void set_status(const QString& profile, const QVector<MapMarker>& markers, CollectionStatus status);
    [[nodiscard]] QVector<DetectionResult> apply_detections(const QString& profile, const RecordingAnalysis& analysis,
        const QString& expected_marker = {});
    void set_retired(const QVector<MapMarker>& markers, bool retired);
    [[nodiscard]] bool can_undo(const QString& profile) const;
    void undo_last(const QString& profile);
    [[nodiscard]] QVector<HistoryEvent> history(const QString& profile, int limit = 250, qint64 before_id = 0) const;
    [[nodiscard]] QVector<HistoryEvent> history_after(const QString& profile, qint64 after_id, int limit = 100) const;
    [[nodiscard]] QByteArray background(const LayerKey& layer) const;
    [[nodiscard]] QString background_credit(const LayerKey& layer) const;
    void set_background(const LayerKey& layer, const QByteArray& png, const QString& credit, const QString& license);
    void backup(const QString& destination) const;
    void restore(const QString& source);

private:
    void migrate();
    void ensure_profile();
    void close() noexcept;
    QString path_;
    QString connection_;
    QSqlDatabase db_;
};

} // namespace teyvat
