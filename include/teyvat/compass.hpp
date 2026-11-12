#pragma once
#include "teyvat/store.hpp"
#include "teyvat/tracking.hpp"
#include "teyvat/window_capture.hpp"
#include "teyvat/official_map.hpp"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QPoint>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <QVariantAnimation>
#include <QKeySequence>
#include <memory>

class QAction;
class QLabel;
class QMenu;
class QSystemTrayIcon;
class QCloseEvent;
class QMouseEvent;
class QPushButton;
class QTemporaryDir;
class QComboBox;
class QToolButton;

namespace teyvat {
class MainWindow;
class CaptureRecorder;
class HoYoLabSync;
struct OfficialRuntime;
struct LiveEstimate {
    LocalizationResult estimate;
    QString error, warning;
    bool cancelled = false, prepared = false;
    std::shared_ptr<OfficialRuntime> official;
    std::optional<TrackingSetup> setup;
    std::optional<LocalizationConfig> reference;
    std::optional<QPointF> anchor_position;
    QString layer;
};

class TargetBeacon final : public QWidget {
    Q_OBJECT
public:
    TargetBeacon();
    void set_target(double bearing, std::optional<double> metres, const QRect& game, bool is_near = false);
protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
private:
    double bearing_ = 0;
    std::optional<double> metres_;
    bool near_ = false;
};

class CompassOverlay final : public QWidget {
    Q_OBJECT
public:
    explicit CompassOverlay();
    void set_editing(bool editing);
    [[nodiscard]] bool editing() const { return editing_; }
    void set_extent(int pixels);
    void place(QPoint position);
    void set_guidance(const std::optional<Guidance>& guidance, bool position_known, const QString& target = {},
        std::optional<double> camera = {}, bool require_camera = false, std::optional<double> metres = {}, bool is_near = false);
signals:
    void moved(QPoint position);
protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    bool nativeEvent(const QByteArray&, void*, qintptr*) override;
private:
    void apply_native_styles();
    bool editing_ = false, dragging_ = false;
    QPoint drag_offset_;
    QVariantAnimation turn_;
    std::optional<double> bearing_;
    double drawn_bearing_ = 0;
    bool position_known_ = false, has_target_ = false;
    bool camera_required_ = false;
    std::optional<double> metres_;
    bool near_ = false;
};

class CompassPanel final : public QWidget {
    Q_OBJECT
public:
    explicit CompassPanel(Store& store);
    ~CompassPanel() override;
    [[nodiscard]] CompassOverlay& overlay() { return overlay_; }
    [[nodiscard]] const TrackingView* tracking_view() const { return tracking_ ? &tracking_->view() : nullptr; }
    void show_test_window();
    void show_navigation_example();
protected:
    void closeEvent(QCloseEvent*) override;
    void changeEvent(QEvent*) override;
    bool nativeEvent(const QByteArray&, void*, qintptr*) override;
private:
    void update_visibility();
    void update_panel_status();
    void set_enabled(bool enabled);
    void save_options();
    void show_tools();
    void present_panel();
    void load_navigation_data();
    void toggle_tracking();
    void stop_tracking();
    void sample_tracking();
    void show_tracking();
    void prepare_game_data();
    void show_target_map();
    void start_official();
    void sample_official();
    void show_official_guidance();
    void confirm_current_target();
    void collection_settings();
    bool register_collect_shortcut(const QKeySequence& shortcut);
    void apply_region_filter();
    [[nodiscard]] QVector<OfficialWaypoint> suggested_teleports() const;
    QString tracking_status() const;
    Store& store_;
    QSettings options_;
    CompassOverlay overlay_;
    TargetBeacon beacon_;
    QTimer timer_;
    QSystemTrayIcon* tray_ = nullptr;
    QMenu* menu_ = nullptr;
    QAction *enabled_action_ = nullptr, *edit_action_ = nullptr;
    QLabel *status_ = nullptr, *notice_ = nullptr, *collect_hint_ = nullptr;
    QPushButton* start_tracking_ = nullptr;
    QComboBox* region_box_ = nullptr;
    QToolButton* settings_button_ = nullptr;
    QWidget* target_area_ = nullptr;
    std::unique_ptr<QWidget> test_window_;
    std::unique_ptr<MainWindow> tools_;
    std::unique_ptr<CaptureRecorder> recorder_;
    std::unique_ptr<HoYoLabSync> hoyolab_;
    std::unique_ptr<QTemporaryDir> demo_directory_;
    std::unique_ptr<Store> demo_store_;
    std::unique_ptr<QLabel> navigation_world_;
    std::optional<TrackingSetup> loaded_setup_;
    std::shared_ptr<OfficialRuntime> official_;
    LocalizationConfig camera_config_;
    LocalizationResult camera_pose_;
    std::optional<CameraHeading> camera_;
    std::optional<bool> last_hud_presence_;
    QString preparing_profile_;
    std::unique_ptr<TrackingSession> tracking_;
    std::optional<WindowSource> tracking_source_;
    QElapsedTimer capture_clock_;
    QFutureWatcher<LiveEstimate> matcher_;
    QImage pending_opening_;
    std::stop_source match_stop_;
    quint64 capture_epoch_ = 0, pending_epoch_ = 0;
    qint64 pending_time_ = -1, last_sample_ = -1000, last_reading_ = -1, last_camera_sample_ = -1000, last_scene_sample_ = -1000;
    bool running_ = false;
    bool recovery_pending_ = true;
    QPoint saved_position_;
    bool enabled_ = true, economy_ = true, writable_ = true, preview_active_ = false;
    int extent_ = 128, opacity_ = 75, official_map_id_ = 2;
    int official_area_id_ = 0;
    bool hotkey_toggle_ = false, hotkey_move_ = false;
    bool hotkey_skip_ = false, hotkey_undo_ = false;
    bool hotkey_collect_ = false;
    int collect_hotkey_id_ = 0x5444;
    QKeySequence collect_shortcut_{Qt::Key_F8};
    double near_distance_ = 20;
    qint64 last_confirm_time_ = -1000;
    std::optional<MarkerKey> planned_target_;
    std::optional<OfficialWaypoint> planned_teleport_;
    qint64 planned_until_ = -1;
};
}
