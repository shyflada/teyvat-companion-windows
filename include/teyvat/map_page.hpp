#pragma once
#include "teyvat/store.hpp"
#include <QFutureWatcher>
#include <QGraphicsView>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QGraphicsEllipseItem;
class QGraphicsPixmapItem;

namespace teyvat {

class MapCanvas final : public QGraphicsView {
    Q_OBJECT
public:
    explicit MapCanvas(QWidget* parent = nullptr);
    ~MapCanvas() override;
    void set_layer(const MapLayer& layer, const QImage& background);
    void set_markers(const QVector<MapMarker>& markers);
    void select_ids(const QStringList& ids);
    void set_position(std::optional<QPointF> position);
    [[nodiscard]] QStringList selected_ids() const;
    void fit_layer();
    void zoom(double factor);
signals:
    void markerSelectionChanged();
    void addAt(QPointF position);
protected:
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
private:
    QRectF bounds_{0, 0, 1000, 1000};
    QVector<QGraphicsEllipseItem*> pins_;
    QGraphicsEllipseItem* position_ = nullptr;
    bool autofit_ = true;
};

struct ParsedImport { Dataset data; QString error; };

class MapPage final : public QWidget {
    Q_OBJECT
public:
    explicit MapPage(Store& store, QWidget* parent = nullptr);
    ~MapPage() override;
    void reload(bool background_changed = false);
    void import_bytes(const QByteArray& bytes, bool csv);
    void analyze_recording_file(const QString& path);
    void set_example_mode();
signals:
    void busyChanged(bool busy);
    void databaseRestored();
protected:
    void changeEvent(QEvent* event) override;
private:
    void render();
    void retranslate();
    void new_layer();
    void edit_marker(bool existing, std::optional<QPointF> point = std::nullopt);
    void import_file();
    void export_file();
    void show_progress();
    void attach_background();
    void backup_file();
    void restore_file();
    void run_task(std::function<void(Store&)> operation, bool restoring = false, bool background_changed = false);
    void set_busy(bool busy);
    void selected_from_map();
    [[nodiscard]] QVector<MapMarker> selected() const;
    [[nodiscard]] std::optional<MapLayer> current_layer() const;
    void error(const QString& message);

    Store& store_;
    QVector<MapLayer> layers_;
    QVector<MapMarker> all_, visible_;
    std::optional<LayerKey> rendered_layer_;
    QFutureWatcher<QString> task_;
    QFutureWatcher<ParsedImport> parser_;
    QFutureWatcher<DetectionRun> detector_;
    std::stop_source detection_stop_;
    Dataset pending_;
    QWidget* controls_ = nullptr;
    QWidget* advanced_ = nullptr;
    MapCanvas* canvas_ = nullptr;
    QComboBox *layer_ = nullptr, *category_ = nullptr, *status_filter_ = nullptr, *new_status_ = nullptr;
    QCheckBox* retired_ = nullptr;
    QLineEdit* search_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel *heading_ = nullptr, *help_ = nullptr, *stats_ = nullptr, *source_ = nullptr, *busy_label_ = nullptr;
    QPushButton *add_ = nullptr, *edit_ = nullptr, *mark_ = nullptr, *undo_ = nullptr, *archive_ = nullptr;
    QPushButton* cancel_analysis_ = nullptr;
    QPushButton *advanced_toggle_ = nullptr, *confirm_collected_ = nullptr;
    QVector<QPair<QPushButton*, const char*>> captions_;
    bool updating_ = false, busy_ = false, restoring_ = false, ready_ = false;
    bool refresh_image_ = false;
    bool has_background_ = false;
};
} // namespace teyvat
