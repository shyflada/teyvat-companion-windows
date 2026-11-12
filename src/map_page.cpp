#include "teyvat/map_page.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGridLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSet>
#include <QScrollBar>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>

namespace teyvat {
namespace {
QString status_text(CollectionStatus value) {
    const char* name = "Unknown";
    switch (value) {
    case CollectionStatus::NotCollected: name = "Not collected"; break;
    case CollectionStatus::PossiblyCollected: name = "Possibly collected"; break;
    case CollectionStatus::Collected: name = "Collected"; break;
    case CollectionStatus::ManuallyConfirmed: name = "Manually confirmed"; break;
    case CollectionStatus::Unknown: break;
    }
    return QCoreApplication::translate("teyvat::MapPage", name);
}
const QVector<CollectionStatus> statuses{CollectionStatus::NotCollected, CollectionStatus::PossiblyCollected,
    CollectionStatus::Collected, CollectionStatus::ManuallyConfirmed, CollectionStatus::Unknown};
QString category_text(const QString& code) {
    const QVector<QPair<QString, const char*>> names{{QStringLiteral("common"), "Common"}, {QStringLiteral("exquisite"), "Exquisite"},
        {QStringLiteral("precious"), "Precious"}, {QStringLiteral("luxurious"), "Luxurious"},
        {QStringLiteral("remarkable"), "Remarkable"}, {QStringLiteral("custom"), "Custom"}};
    for (const auto& item : names) { if (item.first == code) { return QCoreApplication::translate("teyvat::MapPage", item.second); } }
    return code;
}

bool confirm(QWidget* parent, const QString& title, const QString& text) {
    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No, parent);
    box.button(QMessageBox::Yes)->setText(QCoreApplication::translate("teyvat::MapPage", "Yes"));
    box.button(QMessageBox::No)->setText(QCoreApplication::translate("teyvat::MapPage", "No"));
    box.setTextFormat(Qt::PlainText); box.setDefaultButton(QMessageBox::No); return box.exec() == QMessageBox::Yes;
}
QDialogButtonBox* dialog_buttons(QDialogButtonBox::StandardButtons flags) {
    auto* box = new QDialogButtonBox(flags);
    for (const auto& item : {qMakePair(QDialogButtonBox::Ok, "Apply"), qMakePair(QDialogButtonBox::Cancel, "Cancel"), qMakePair(QDialogButtonBox::Close, "Close")}) {
        if (auto* button = box->button(item.first)) { button->setText(QCoreApplication::translate("teyvat::MapPage", item.second)); }
    }
    return box;
}
QLabel* plain(const QString& text = {}) {
    auto* label = new QLabel(text); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); return label;
}
QByteArray bounded_read(const QString& path, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) { throw std::runtime_error("Cannot open file, or file exceeds its size limit."); }
    auto bytes = file.read(limit + 1);
    if (bytes.size() > limit || file.error() != QFileDevice::NoError) { throw std::runtime_error("File read failed or exceeded the size limit."); }
    return bytes;
}
} // namespace

MapCanvas::MapCanvas(QWidget* parent) : QGraphicsView(parent) {
    setObjectName(QStringLiteral("mapCanvas"));
    setScene(new QGraphicsScene(this)); setDragMode(ScrollHandDrag);
    setTransformationAnchor(AnchorUnderMouse); setResizeAnchor(AnchorViewCenter);
    // fitInView inside resizeEvent must not toggle scrollbars and resize forever.
    // Panning still uses ScrollHandDrag and the hidden scrollbar values.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    setMinimumSize(320, 240); setFocusPolicy(Qt::StrongFocus);
    connect(scene(), &QGraphicsScene::selectionChanged, this, &MapCanvas::markerSelectionChanged);
}

MapCanvas::~MapCanvas() {
    // Clearing selected items during scene destruction must not notify consumers
    // whose other child widgets may already have been destroyed.
    disconnect(scene(), nullptr, this, nullptr);
}

void MapCanvas::set_layer(const MapLayer& layer, const QImage& image) {
    const QSignalBlocker block(scene()); scene()->clear(); pins_.clear(); position_ = nullptr;
    bounds_ = QRectF(0, 0, layer.width, layer.height);
    scene()->setSceneRect(bounds_.adjusted(-layer.width * .03, -layer.height * .03, layer.width * .03, layer.height * .03));
    if (!image.isNull()) {
        auto* background = scene()->addPixmap(QPixmap::fromImage(image));
        background->setTransform(QTransform::fromScale(layer.width / image.width(), layer.height / image.height()));
        background->setZValue(-1);
    }
    fit_layer();
}

void MapCanvas::set_markers(const QVector<MapMarker>& markers) {
    const QSignalBlocker block(scene());
    for (auto* pin : pins_) { delete pin; } pins_.clear();
    for (const auto& marker : markers) {
        QColor color = is_confirmed(marker.status) ? QColor("#83d7ba")
            : marker.status == CollectionStatus::PossiblyCollected ? QColor("#f0c66d")
            : marker.status == CollectionStatus::Unknown ? QColor("#9daaba") : QColor("#79b7ef");
        if (marker.retired) { color.setAlpha(100); }
        auto* pin = scene()->addEllipse(-6, -6, 12, 12, QPen(QColor("#e9f1f5"), 1.2), QBrush(color));
        pin->setPos(marker.x, marker.y); pin->setZValue(1);
        pin->setFlags(QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemIgnoresTransformations);
        pin->setData(0, marker.id);
        pin->setToolTip(QStringLiteral("<qt>") + marker.label.toHtmlEscaped() + QStringLiteral("<br>") + marker.id.toHtmlEscaped() + QStringLiteral(" · %1, %2</qt>").arg(marker.x).arg(marker.y));
        pins_.append(pin);
    }
}

QStringList MapCanvas::selected_ids() const {
    QStringList ids; for (const auto* item : scene()->selectedItems()) { ids.append(item->data(0).toString()); } return ids;
}
void MapCanvas::select_ids(const QStringList& ids) {
    const QSignalBlocker block(scene());
    const QSet<QString> selected(ids.cbegin(), ids.cend());
    for (auto* pin : pins_) { pin->setSelected(selected.contains(pin->data(0).toString())); }
}
void MapCanvas::set_position(std::optional<QPointF> position) {
    delete position_; position_ = nullptr;
    if (!position || !bounds_.contains(*position)) { return; }
    position_ = scene()->addEllipse(-10, -10, 20, 20, QPen(QColor("#e7b85e"), 2.5), Qt::NoBrush);
    position_->setFlags(QGraphicsItem::ItemIgnoresTransformations); position_->setZValue(3); position_->setPos(*position);
    position_->setData(1, QStringLiteral("recordedPosition"));
    position_->setToolTip(QCoreApplication::translate("teyvat::MapPage", "Position estimated from the selected recording"));
}
void MapCanvas::fit_layer() { autofit_ = true; fitInView(bounds_, Qt::KeepAspectRatio); }
void MapCanvas::zoom(double factor) {
    const auto next = transform().m11() * factor;
    if (next >= .00001 && next <= 100) { autofit_ = false; scale(factor, factor); }
}
void MapCanvas::wheelEvent(QWheelEvent* event) {
    if (event->angleDelta().y() == 0) { QGraphicsView::wheelEvent(event); return; }
    zoom(event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2); event->accept();
}
void MapCanvas::resizeEvent(QResizeEvent* event) { QGraphicsView::resizeEvent(event); if (autofit_) { fitInView(bounds_, Qt::KeepAspectRatio); } }
void MapCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) { zoom(1.2); }
    else if (event->key() == Qt::Key_Minus) { zoom(1.0 / 1.2); }
    else if (event->key() == Qt::Key_F) { fit_layer(); }
    else { QGraphicsView::keyPressEvent(event); }
}
void MapCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    const auto position = mapToScene(event->position().toPoint());
    if (event->button() == Qt::LeftButton && bounds_.contains(position)) { emit addAt(position); event->accept(); }
    else { QGraphicsView::mouseDoubleClickEvent(event); }
}
void MapCanvas::drawBackground(QPainter* painter, const QRectF& rect) {
    painter->fillRect(rect, QColor("#111c26"));
    const double scale_value = std::max(.00001, transform().m11());
    const double step = std::pow(10.0, std::ceil(std::log10(45.0 / scale_value)));
    QPen pen(QColor("#263b48")); pen.setCosmetic(true); painter->setPen(pen);
    const auto visible = rect.intersected(bounds_);
    for (double x = std::ceil(visible.left() / step) * step; x <= visible.right(); x += step) { painter->drawLine(QPointF(x, visible.top()), QPointF(x, visible.bottom())); }
    for (double y = std::ceil(visible.top() / step) * step; y <= visible.bottom(); y += step) { painter->drawLine(QPointF(visible.left(), y), QPointF(visible.right(), y)); }
    pen.setColor(QColor("#547080")); painter->setPen(pen); painter->drawRect(bounds_);
}

MapPage::MapPage(Store& store, QWidget* parent) : QWidget(parent), store_(store) {
    setObjectName(QStringLiteral("mapPage"));
    auto* root = new QVBoxLayout(this); root->setContentsMargins(24, 20, 24, 20); root->setSpacing(10);
    heading_ = plain(); heading_->setProperty("kind", QStringLiteral("title")); root->addWidget(heading_);
    help_ = plain(); help_->setProperty("kind", QStringLiteral("muted")); root->addWidget(help_);
    controls_ = new QWidget; auto* layout = new QVBoxLayout(controls_); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(10); root->addWidget(controls_, 1);
    auto* primary = new QGridLayout; primary->setObjectName(QStringLiteral("primaryActions"));
    advanced_ = new QWidget; advanced_->setObjectName(QStringLiteral("advancedTools")); auto* tools = new QGridLayout(advanced_); tools->setContentsMargins(0, 0, 0, 0);
    int advanced_count = 0, basic_count = 0;
    const auto action = [&](const char* caption, const char* name, bool basic, const std::function<void()>& callback) {
        auto* button = new QPushButton; button->setObjectName(QString::fromLatin1(name));
        captions_.append({button, caption});
        if (basic) { primary->addWidget(button, basic_count / 2, basic_count % 2); ++basic_count; } else { tools->addWidget(button, advanced_count / 3, advanced_count % 3); ++advanced_count; }
        connect(button, &QPushButton::clicked, this, callback); return button;
    };
    action("Open prepared recording", "analyzeRecording", true, [this] {
        if (!current_layer()) { error(tr("Add the recording's map under More tools, or try the example from Start.")); return; }
        const auto path = QFileDialog::getOpenFileName(this, tr("Choose an offline recording"), {}, tr("Recording manifest (*.json)"));
        if (!path.isEmpty()) { analyze_recording_file(path); }
    });
    undo_ = action("Undo last change", "undoProgress", true, [this] {
        const auto profile = store_.active_profile().id; run_task([profile](Store& db) { db.undo_last(profile); });
    });
    action("Progress / history", "showProgress", true, [this] { show_progress(); });
    advanced_toggle_ = new QPushButton; advanced_toggle_->setObjectName(QStringLiteral("showAdvanced")); advanced_toggle_->setCheckable(true);
    captions_.append({advanced_toggle_, "More tools"}); advanced_toggle_->setProperty("disclosure", true); primary->addWidget(advanced_toggle_, 1, 1); layout->addLayout(primary);
    action("New layer", "newMap", false, [this] { new_layer(); });
    add_ = action("Add marker", "addMarker", false, [this] { edit_marker(false); });
    edit_ = action("Edit marker", "editMarker", false, [this] { edit_marker(true); });
    action("Import JSON / CSV", "importMarkers", false, [this] { import_file(); });
    action("Export layer", "exportMarkers", false, [this] { export_file(); });
    action("Backup database", "backupDatabase", false, [this] { backup_file(); });
    action("Restore backup", "restoreDatabase", false, [this] { restore_file(); });
    action("PNG background", "attachBackground", false, [this] { attach_background(); });
    action("Clear background", "clearBackground", false, [this] {
        if (const auto map = current_layer()) { run_task([key = map->key](Store& db) { db.set_background(key, {}, {}, {}); }, false, true); }
    });
    archive_ = action("Archive / restore", "archiveMarkers", false, [this] {
        const auto values = selected(); if (values.isEmpty()) { return; }
        const bool retire = std::any_of(values.begin(), values.end(), [](const auto& m) { return !m.retired; });
        if (confirm(this, tr("Shared markers"), tr("Change archive state for the selected markers in all local profiles? Progress is retained. Show archived markers to restore them."))) {
            run_task([values, retire](Store& db) { db.set_retired(values, retire); });
        }
    });
    auto* recording_note = plain(tr("Recording mode · Progress saves automatically · Live game tracking is still in development"));
    recording_note->setObjectName(QStringLiteral("recordingNotice")); recording_note->setProperty("kind", QStringLiteral("muted")); layout->addWidget(recording_note);
    layout->addWidget(advanced_); advanced_->hide();
    auto* filters = new QGridLayout;
    layer_ = new QComboBox; layer_->setObjectName(QStringLiteral("mapLayer")); filters->addWidget(layer_, 0, 0, 1, 2);
    layer_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); layer_->setMinimumContentsLength(12);
    search_ = new QLineEdit; search_->setObjectName(QStringLiteral("mapSearch")); search_->setMaxLength(200); filters->addWidget(search_, 0, 2, 1, 2);
    category_ = new QComboBox; category_->setObjectName(QStringLiteral("categoryFilter")); filters->addWidget(category_, 1, 0);
    category_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); category_->setMinimumContentsLength(8);
    status_filter_ = new QComboBox; status_filter_->setObjectName(QStringLiteral("statusFilter")); filters->addWidget(status_filter_, 1, 1);
    retired_ = new QCheckBox; retired_->setObjectName(QStringLiteral("showArchived")); tools->addWidget(retired_, advanced_count / 3, advanced_count % 3); ++advanced_count;
    auto* fit = new QPushButton; fit->setObjectName(QStringLiteral("fitMap")); captions_.append({fit, "Fit map (F)"}); filters->addWidget(fit, 1, 2, 1, 2);
    layout->addLayout(filters);
    stats_ = plain(); stats_->setObjectName(QStringLiteral("mapStats")); layout->addWidget(stats_);
    auto* split = new QSplitter(Qt::Horizontal);
    canvas_ = new MapCanvas; split->addWidget(canvas_);
    canvas_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    table_ = new QTableWidget(0, 3); table_->setObjectName(QStringLiteral("markerTable"));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->setMinimumSize(280, 240); table_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    split->addWidget(table_); split->setSizes({580, 420}); split->setChildrenCollapsible(false); layout->addWidget(split, 1);
    auto* collect = new QHBoxLayout;
    new_status_ = new QComboBox; new_status_->setObjectName(QStringLiteral("collectionStatus")); collect->addWidget(new_status_);
    mark_ = new QPushButton; mark_->setObjectName(QStringLiteral("applyStatus")); captions_.append({mark_, "Apply to selected"}); collect->addWidget(mark_);
    const int extra_row = (advanced_count + 2) / 3; tools->addLayout(collect, extra_row, 0, 1, 3);
    source_ = plain(); source_->setProperty("kind", QStringLiteral("muted")); tools->addWidget(source_, extra_row + 1, 0, 1, 3);
    confirm_collected_ = new QPushButton; confirm_collected_->setObjectName(QStringLiteral("confirmCollected")); captions_.append({confirm_collected_, "Confirm collected"}); layout->addWidget(confirm_collected_);
    connect(confirm_collected_, &QPushButton::clicked, this, [this] {
        const auto values = selected(); const auto profile = store_.active_profile().id;
        if (values.size() == 1) { run_task([values, profile](Store& db) { db.set_status(profile, values, CollectionStatus::ManuallyConfirmed); }); }
    });
    connect(advanced_toggle_, &QPushButton::toggled, advanced_, &QWidget::setVisible);
    cancel_analysis_ = new QPushButton(tr("Cancel analysis")); cancel_analysis_->setObjectName(QStringLiteral("cancelAnalysis")); cancel_analysis_->hide(); root->addWidget(cancel_analysis_);
    connect(cancel_analysis_, &QPushButton::clicked, this, [this] { detection_stop_.request_stop(); cancel_analysis_->setEnabled(false); });
    busy_label_ = plain(); root->addWidget(busy_label_);

    connect(fit, &QPushButton::clicked, canvas_, &MapCanvas::fit_layer);
    connect(layer_, &QComboBox::currentIndexChanged, this, [this] { if (!updating_) { reload(); } });
    connect(category_, &QComboBox::currentIndexChanged, this, [this] { if (!updating_) { render(); } });
    connect(status_filter_, &QComboBox::currentIndexChanged, this, [this] { if (!updating_) { render(); } });
    connect(retired_, &QCheckBox::toggled, this, [this] { render(); });
    auto* debounce = new QTimer(this); debounce->setSingleShot(true); debounce->setInterval(150);
    connect(search_, &QLineEdit::textChanged, debounce, [debounce] { debounce->start(); });
    connect(debounce, &QTimer::timeout, this, &MapPage::render);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        if (updating_) { return; }
        QStringList ids; for (const auto& marker : selected()) { ids.append(marker.id); }
        canvas_->select_ids(ids); edit_->setEnabled(ids.size() == 1); mark_->setEnabled(!ids.isEmpty()); confirm_collected_->setEnabled(ids.size() == 1); archive_->setEnabled(!ids.isEmpty());
    });
    connect(canvas_, &MapCanvas::markerSelectionChanged, this, &MapPage::selected_from_map);
    connect(canvas_, &MapCanvas::addAt, this, [this](QPointF point) {
        if (!busy_ && current_layer()) { if (advanced_toggle_->isChecked()) { edit_marker(false, point); } else { canvas_->zoom(1.5); } }
    });
    connect(mark_, &QPushButton::clicked, this, [this] {
        const auto values = selected(); const auto profile = store_.active_profile().id;
        const auto status = read_status(new_status_->currentData().toString());
        if (!values.isEmpty()) { run_task([values, profile, status](Store& db) { db.set_status(profile, values, status); }); }
    });
    connect(&task_, &QFutureWatcher<QString>::finished, this, [this] {
        const auto message = task_.result(); set_busy(false);
        if (!message.isEmpty()) { error(message); return; }
        if (restoring_) { emit databaseRestored(); }
        reload(refresh_image_ || restoring_);
    });
    connect(&parser_, &QFutureWatcher<ParsedImport>::finished, this, [this] {
        auto result = parser_.result(); set_busy(false);
        if (!result.error.isEmpty()) { error(result.error); return; }
        pending_ = std::move(result.data);
        QDialog dialog(this); dialog.setObjectName(QStringLiteral("importReview")); dialog.setWindowTitle(tr("Review import"));
        auto* box = new QVBoxLayout(&dialog);
        box->addWidget(plain(tr("Marker and layer metadata is shared by local profiles. Progress is imported into the active profile only. The file must have a permitted source and usage rights.")));
        auto* policy = new QCheckBox(tr("Replace existing local progress with statuses from the file")); policy->setObjectName(QStringLiteral("overwriteProgress")); box->addWidget(policy);
        auto* summary = plain(); box->addWidget(summary);
        auto* buttons = dialog_buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); box->addWidget(buttons);
        QByteArray token;
        const auto profile = store_.active_profile().id;
        const auto review = [&] {
            try {
                const auto plan = store_.review_import(pending_, profile, policy->isChecked()); token = plan.token;
                summary->setText(tr("New layers: %1\nNew markers: %2\nChanged marker metadata: %3\nProgress changes: %4\n\nExisting progress is preserved unless replacement is selected.")
                    .arg(plan.new_maps).arg(plan.new_markers).arg(plan.updated_markers).arg(plan.progress_changes));
                buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
            } catch (const std::exception& ex) { summary->setText(QString::fromUtf8(ex.what())); buttons->button(QDialogButtonBox::Ok)->setEnabled(false); }
        };
        connect(policy, &QCheckBox::toggled, &dialog, review); connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        review();
        if (dialog.exec() == QDialog::Accepted) {
            const auto data = pending_; const bool overwrite = policy->isChecked();
            run_task([data, profile, overwrite, token](Store& db) { db.apply_import(data, profile, overwrite, token); });
        }
        pending_ = {};
    });
    connect(&detector_, &QFutureWatcher<DetectionRun>::finished, this, [this] {
        const auto run = detector_.result(); set_busy(false);
        if (run.cancelled) { busy_label_->setText(tr("Analysis cancelled. No progress was changed.")); return; }
        if (!run.error.isEmpty()) { error(run.error); return; }
        reload();
        if (!run.analysis.localizations.isEmpty()) {
            const auto& position = run.analysis.localizations.last().estimate.position;
            canvas_->set_position(position ? std::optional<QPointF>(QPointF(position->x, position->y)) : std::nullopt);
            source_->setText(source_->text() + QLatin1Char('\n') + tr("Displayed position is from recording time %1 ms, not live tracking.").arg(run.analysis.localizations.last().time_ms));
        }
        QDialog dialog(this); dialog.setObjectName(QStringLiteral("detectionResults")); dialog.setWindowTitle(tr("Offline analysis results")); dialog.resize(900, 620);
        auto* layout = new QVBoxLayout(&dialog);
        int applied = 0, review = 0, duplicates = 0, unmatched = 0;
        for (const auto& item : run.results) { applied += item.outcome == DetectionOutcome::Applied; review += item.outcome == DetectionOutcome::Review; duplicates += item.outcome == DetectionOutcome::Duplicate; unmatched += item.outcome == DetectionOutcome::Unmatched; }
        auto* summary = plain(tr("Automatically collected: %1\nNeeds review: %2").arg(applied).arg(review));
        if (duplicates) { summary->setText(summary->text() + QLatin1Char('\n') + tr("Already processed: %1. Previous undo actions are preserved.").arg(duplicates)); }
        if (run.results.isEmpty()) { summary->setText(tr("No chest-opening events found in this recording.")); }
        summary->setObjectName(QStringLiteral("detectionSummary")); layout->addWidget(summary);
        summary->setProperty("kind", QStringLiteral("heading"));
        layout->addWidget(plain(tr("Research preview. Accuracy in Genshin has not been verified.")));
        auto* details_toggle = new QPushButton(tr("Show details")); details_toggle->setObjectName(QStringLiteral("showDetectionDetails")); details_toggle->setCheckable(true); details_toggle->setProperty("disclosure", true); layout->addWidget(details_toggle);
        auto* details = new QWidget; details->setObjectName(QStringLiteral("detectionDetails")); auto* detail_layout = new QVBoxLayout(details); detail_layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(details, 1);
        detail_layout->addWidget(plain(tr("Frames: %1 · Opening events: %2").arg(run.analysis.frame_count).arg(run.results.size())));
        if (!run.analysis.localizations.isEmpty()) {
            int located = 0; for (const auto& item : run.analysis.localizations) { located += item.estimate.position.has_value(); }
            auto* label = plain(tr("Positions found from images: %1 / %2. A missing or ambiguous position prevents automatic collection.").arg(located).arg(run.analysis.localizations.size()));
            label->setObjectName(QStringLiteral("localizationSummary")); detail_layout->addWidget(label);
        }
        auto* tabs = new QTabWidget; detail_layout->addWidget(tabs, 1);
        auto* events = new QTableWidget(static_cast<int>(run.results.size()), 4); events->setObjectName(QStringLiteral("detectionEvents"));
        events->setHorizontalHeaderLabels({tr("Time (ms)"), tr("Result"), tr("Possible markers"), tr("Evidence / reason")});
        events->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); events->horizontalHeader()->setStretchLastSection(true);
        events->setSelectionBehavior(QAbstractItemView::SelectRows); events->setSelectionMode(QAbstractItemView::SingleSelection); events->setEditTriggers(QAbstractItemView::NoEditTriggers);
        for (int i = 0; i < run.results.size(); ++i) {
            const auto& item = run.results[i]; QString outcome, reason;
            switch (item.outcome) {
            case DetectionOutcome::Applied: outcome = tr("Automatically collected"); break;
            case DetectionOutcome::Review: outcome = tr("Needs review"); break;
            case DetectionOutcome::Unmatched: outcome = tr("No change"); break;
            case DetectionOutcome::AlreadyCollected: outcome = tr("Already collected"); break;
            case DetectionOutcome::Duplicate: outcome = tr("Duplicate ignored"); break;
            }
            switch (item.reason) {
            case DetectionReason::UniqueMatch: reason = tr("Stable opening and a unique marker"); break;
            case DetectionReason::AmbiguousMarkers: reason = tr("Multiple nearby markers"); break;
            case DetectionReason::UncertainPosition: reason = tr("Uncertain or discontinuous position"); break;
            case DetectionReason::NoPosition: reason = tr("Position missing or outside map bounds"); break;
            case DetectionReason::NoMarker: reason = tr("No nearby marker"); break;
            case DetectionReason::ArchivedMarker: reason = tr("Marker is archived"); break;
            case DetectionReason::UnknownCategory: reason = tr("Category is not a known chest type"); break;
            case DetectionReason::AlreadyCollected: reason = tr("Confirmed status preserved"); break;
            case DetectionReason::PreviouslyProcessed: reason = tr("Previously processed; Undo is respected"); break;
            case DetectionReason::TargetMismatch: reason = tr("Opening does not match the locked target"); break;
            }
            QStringList names;
            for (const auto& id : item.candidates) {
                const auto marker = std::find_if(all_.cbegin(), all_.cend(), [&](const auto& value) { return value.id == id; });
                names.append(marker == all_.cend() ? tr("Unnamed marker") : marker->label);
            }
            const QStringList values{QString::number(item.time_ms), outcome, names.join(QStringLiteral(", ")), reason};
            for (int j = 0; j < values.size(); ++j) { events->setItem(i, j, new QTableWidgetItem(values[j])); }
        }
        tabs->addTab(events, tr("Opening events"));
        if (!run.analysis.localizations.isEmpty()) {
            auto* positions = new QTableWidget(static_cast<int>(run.analysis.localizations.size()), 5); positions->setObjectName(QStringLiteral("localizationResults"));
            positions->setHorizontalHeaderLabels({tr("Time (ms)"), tr("Position in local units"), tr("Similarity"), tr("Alternative score"), tr("Result")});
            positions->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); positions->horizontalHeader()->setStretchLastSection(true);
            positions->setEditTriggers(QAbstractItemView::NoEditTriggers);
            for (int i = 0; i < run.analysis.localizations.size(); ++i) {
                const auto& sample = run.analysis.localizations[i]; const auto& estimate = sample.estimate; QString reason;
                switch (estimate.reason) {
                case LocalizationReason::Matched: reason = tr("Position found"); break;
                case LocalizationReason::LowTexture: reason = tr("Insufficient map detail"); break;
                case LocalizationReason::WeakMatch: reason = tr("No reliable visual match"); break;
                case LocalizationReason::Ambiguous: reason = tr("Multiple similar map locations"); break;
                case LocalizationReason::OutsideReference: reason = tr("Minimap does not fit reference coverage"); break;
                case LocalizationReason::MinimapAbsent: reason = tr("Minimap is hidden or its HUD changed"); break;
                case LocalizationReason::InsufficientFeatures: reason = tr("Not enough unique map landmarks"); break;
                case LocalizationReason::InconsistentGeometry: reason = tr("Map landmark geometry is unreliable"); break;
                }
                const QString point = estimate.position ? QStringLiteral("%1, %2").arg(estimate.position->x, 0, 'f', 2).arg(estimate.position->y, 0, 'f', 2) : tr("Position unknown");
                const QStringList values{QString::number(sample.time_ms), point, QString::number(estimate.similarity, 'f', 3), QString::number(estimate.runner_up, 'f', 3), reason};
                for (int j = 0; j < values.size(); ++j) { positions->setItem(i, j, new QTableWidgetItem(values[j])); }
            }
            tabs->addTab(positions, tr("Positions from images"));
        }
        layout->addWidget(plain(tr("Progress saves automatically. You can undo a mistaken change or review uncertain markers on the map.")));
        auto* inspect = new QPushButton(tr("Review selected candidates on map")); inspect->setObjectName(QStringLiteral("reviewDetection")); inspect->setEnabled(false); inspect->setVisible(review || unmatched); layout->addWidget(inspect);
        connect(events, &QTableWidget::itemSelectionChanged, &dialog, [events, inspect, &run] {
            const int row = events->currentRow(); inspect->setEnabled(row >= 0 && row < run.results.size() && !run.results[row].candidates.isEmpty());
        });
        connect(inspect, &QPushButton::clicked, &dialog, [&] {
            const int row = events->currentRow(); if (row < 0 || row >= run.results.size()) { return; }
            search_->clear(); category_->setCurrentIndex(0); status_filter_->setCurrentIndex(0); retired_->setChecked(true); render();
            canvas_->select_ids(run.results[row].candidates); selected_from_map(); dialog.accept();
        });
        connect(details_toggle, &QPushButton::toggled, details, &QWidget::setVisible);
        const bool needs_attention = review || unmatched; details->setVisible(needs_attention); details_toggle->setChecked(needs_attention);
        if (needs_attention) {
            for (int i = 0; i < run.results.size(); ++i) {
                if (run.results[i].outcome == DetectionOutcome::Review || run.results[i].outcome == DetectionOutcome::Unmatched) { events->setCurrentCell(i, 0); events->selectRow(i); break; }
            }
        }
        if (applied) {
            auto* undo = new QPushButton(tr("Undo these changes")); undo->setObjectName(QStringLiteral("undoDetection")); layout->addWidget(undo);
            connect(undo, &QPushButton::clicked, &dialog, [this, &dialog] {
                const auto profile = store_.active_profile().id; dialog.accept(); run_task([profile](Store& db) { db.undo_last(profile); });
            });
        }
        auto* close = dialog_buttons(QDialogButtonBox::Close); close->button(QDialogButtonBox::Close)->setText(tr("Back to map")); layout->addWidget(close); connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (!needs_attention) { dialog.resize(560, 300); }
        dialog.exec();
    });
    ready_ = true; retranslate(); reload();
}

MapPage::~MapPage() { detection_stop_.request_stop(); task_.waitForFinished(); parser_.waitForFinished(); detector_.waitForFinished(); }

void MapPage::set_example_mode() {
    advanced_toggle_->setChecked(false); advanced_toggle_->hide();
    findChild<QPushButton*>(QStringLiteral("analyzeRecording"))->hide();
    auto* primary = findChild<QGridLayout*>(QStringLiteral("primaryActions"));
    primary->addWidget(undo_, 0, 0); primary->addWidget(findChild<QPushButton*>(QStringLiteral("showProgress")), 0, 1);
}

std::optional<MapLayer> MapPage::current_layer() const {
    const auto key = layer_->currentData().toString();
    for (const auto& map : layers_) { if (layer_identity(map.key) == key) { return map; } } return std::nullopt;
}
QVector<MapMarker> MapPage::selected() const {
    QVector<MapMarker> result;
    for (const auto& index : table_->selectionModel()->selectedRows()) {
        if (index.row() >= 0 && index.row() < visible_.size()) { result.append(visible_[index.row()]); }
    }
    return result;
}
void MapPage::selected_from_map() {
    if (updating_) { return; }
    updating_ = true;
    const auto ids = canvas_->selected_ids(); table_->clearSelection();
    for (int row = 0; row < visible_.size(); ++row) {
        if (ids.contains(visible_[row].id)) { table_->selectionModel()->select(table_->model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows); }
    }
    edit_->setEnabled(ids.size() == 1); mark_->setEnabled(!ids.isEmpty()); confirm_collected_->setEnabled(ids.size() == 1); archive_->setEnabled(!ids.isEmpty()); updating_ = false;
}

void MapPage::reload(bool background_changed) {
    if (!ready_) { return; }
    canvas_->set_position(std::nullopt);
    try {
        const auto previous = layer_->currentData().toString(); updating_ = true;
        layers_ = store_.layers(); layer_->clear();
        for (const auto& map : layers_) { layer_->addItem(map.title + QStringLiteral(" · ") + map.key.region + QStringLiteral(" / ") + map.key.layer, layer_identity(map.key)); }
        const int index = layer_->findData(previous); layer_->setCurrentIndex(index >= 0 ? index : (layers_.isEmpty() ? -1 : 0));
        const auto map = current_layer(); all_.clear();
        if (map) {
            all_ = store_.markers(store_.active_profile().id, map->key);
            if (background_changed || !rendered_layer_ || *rendered_layer_ != map->key) {
                const auto png = store_.background(map->key);
                has_background_ = !png.isEmpty();
                canvas_->set_layer(*map, png.isEmpty() ? QImage{} : decode_background(png)); rendered_layer_ = map->key;
            }
            source_->setText(tr("Local map units; X right, Y down. Source: %1 · %2 · %3").arg(map->provenance, map->license, map->version));
            if (has_background_) { source_->setText(source_->text() + QStringLiteral("\n") + store_.background_credit(map->key)); }
        } else {
            canvas_->set_layer(MapLayer{}, {}); rendered_layer_.reset(); source_->setText(tr("Create a layer or import your permitted dataset to begin. No official map or chest database is bundled."));
        }
        const auto category = category_->currentData().toString(); category_->clear(); category_->addItem(tr("All categories"), QString());
        QStringList categories; for (const auto& marker : all_) { if (!categories.contains(marker.category)) { categories.append(marker.category); } } categories.sort();
        for (const auto& value : categories) { category_->addItem(category_text(value), value); }
        category_->setCurrentIndex(std::max(0, category_->findData(category)));
        updating_ = false; render();
    } catch (const std::exception& ex) { updating_ = false; error(QString::fromUtf8(ex.what())); }
}

void MapPage::render() {
    if (!ready_ || updating_) { return; }
    QStringList selected_ids; for (const auto& marker : selected()) { selected_ids.append(marker.id); }
    updating_ = true; visible_.clear();
    int total = 0, confirmed = 0, uncertain = 0;
    for (const auto& marker : all_) {
        if (!marker.retired) { ++total; confirmed += is_confirmed(marker.status) ? 1 : 0; uncertain += marker.status == CollectionStatus::PossiblyCollected ? 1 : 0; }
        if ((!retired_->isChecked() && marker.retired)
            || (!category_->currentData().toString().isEmpty() && marker.category != category_->currentData().toString())
            || (!status_filter_->currentData().toString().isEmpty() && stored_status(marker.status) != status_filter_->currentData().toString())) { continue; }
        const auto term = search_->text().trimmed();
        // ponytail: linear scan is bounded to 5000 markers/layer; add indexed search only after profiling.
        if (!term.isEmpty() && !marker.label.contains(term, Qt::CaseInsensitive) && !marker.notes.contains(term, Qt::CaseInsensitive) && !marker.id.contains(term, Qt::CaseInsensitive)) { continue; }
        visible_.append(marker);
    }
    table_->setUpdatesEnabled(false); table_->clearSelection(); table_->setRowCount(static_cast<int>(visible_.size()));
    for (int row = 0; row < visible_.size(); ++row) {
        const auto& marker = visible_[row];
        table_->setItem(row, 0, new QTableWidgetItem(marker.label + (marker.retired ? QStringLiteral(" ⊘") : QString())));
        table_->setItem(row, 1, new QTableWidgetItem(category_text(marker.category)));
        table_->setItem(row, 2, new QTableWidgetItem(status_text(marker.status)));
        table_->item(row, 0)->setToolTip(QStringLiteral("<qt>") + marker.label.toHtmlEscaped() + QStringLiteral("<br>") + marker.id.toHtmlEscaped() + QStringLiteral(" · %1, %2</qt>").arg(marker.x).arg(marker.y));
        if (selected_ids.contains(marker.id)) { table_->selectionModel()->select(table_->model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows); }
    }
    table_->setUpdatesEnabled(true); table_->horizontalScrollBar()->setValue(0); table_->viewport()->update();
    canvas_->set_markers(visible_); canvas_->select_ids(selected_ids);
    stats_->setText(tr("Confirmed %1 / %2 · Uncertain %3 · Visible %4. Progress is from this local layer only.").arg(confirmed).arg(total).arg(uncertain).arg(visible_.size()));
    const auto values = selected(); add_->setEnabled(current_layer().has_value()); edit_->setEnabled(values.size() == 1);
    for (const auto* name : {"exportMarkers", "attachBackground", "clearBackground"}) { findChild<QPushButton*>(QString::fromLatin1(name))->setEnabled(current_layer().has_value()); }
    mark_->setEnabled(!values.isEmpty()); confirm_collected_->setEnabled(values.size() == 1); archive_->setEnabled(!values.isEmpty()); undo_->setEnabled(store_.can_undo(store_.active_profile().id));
    updating_ = false;
}

void MapPage::set_busy(bool busy) { busy_ = busy; controls_->setEnabled(!busy); if (!busy) { cancel_analysis_->hide(); } busy_label_->setText(busy ? tr("Working locally…") : QString()); emit busyChanged(busy); }
void MapPage::error(const QString& message) {
    QMessageBox box(QMessageBox::Warning, tr("Operation could not be completed"), message, QMessageBox::Ok, this); box.setTextFormat(Qt::PlainText); box.exec();
}
void MapPage::run_task(std::function<void(Store&)> operation, bool restoring, bool background_changed) {
    if (busy_) { return; } restoring_ = restoring; refresh_image_ = background_changed; set_busy(true);
    const auto path = store_.path();
    task_.setFuture(QtConcurrent::run([path, operation] {
        try { Store db(path); operation(db); return QString{}; }
        catch (const std::exception& ex) { return QString::fromUtf8(ex.what()); }
    }));
}

void MapPage::analyze_recording_file(const QString& recording_path) {
    if (busy_ || !current_layer()) { return; }
    const auto key = current_layer()->key;
    const auto path = store_.path(), profile = store_.active_profile().id;
    detection_stop_ = std::stop_source{}; const auto stop = detection_stop_.get_token();
    set_busy(true); cancel_analysis_->setEnabled(true); cancel_analysis_->show();
    detector_.setFuture(QtConcurrent::run([path, profile, key, recording_path, stop] {
        DetectionRun run;
        try {
            run.analysis = analyze_recording(recording_path, stop);
            if (run.analysis.map != key) { throw std::runtime_error("Select the recording's map layer before analysis. No progress was changed."); }
            if (stop.stop_requested()) { throw std::runtime_error("Analysis cancelled."); }
            Store db(path); run.results = db.apply_detections(profile, run.analysis);
        } catch (const std::exception& ex) { if (stop.stop_requested()) { run.cancelled = true; } else { run.error = QString::fromUtf8(ex.what()); } }
        return run;
    }));
}

void MapPage::new_layer() {
    QDialog dialog(this); dialog.setObjectName(QStringLiteral("layerDialog")); dialog.setWindowTitle(tr("New local map layer"));
    auto* form = new QFormLayout(&dialog);
    auto* title = new QLineEdit; title->setObjectName(QStringLiteral("layerTitle")); title->setMaxLength(120);
    auto* region = new QLineEdit(tr("Local region")); region->setObjectName(QStringLiteral("layerRegion")); region->setMaxLength(128);
    auto* layer = new QLineEdit(tr("Surface")); layer->setObjectName(QStringLiteral("layerName")); layer->setMaxLength(128);
    auto* width = new QDoubleSpinBox; auto* height = new QDoubleSpinBox;
    for (auto* value : {width, height}) { value->setRange(1, 1e6); value->setValue(1000); }
    form->addRow(tr("Map name"), title); form->addRow(tr("Region"), region); form->addRow(tr("Map level"), layer);
    form->addRow(tr("Width in local units"), width); form->addRow(tr("Height in local units"), height);
    form->addRow(plain(tr("This creates a coordinate canvas without official geography or chest data. Coordinates are local units, not metres.")));
    auto* buttons = dialog_buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        const MapLayer map{{QStringLiteral("local"), region->text().trimmed(), layer->text().trimmed()}, title->text().trimmed(),
            QStringLiteral("local:") + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(":v1"), width->value(), height->value()};
        run_task([map](Store& db) { const auto maps = db.layers(); for (const auto& old : maps) { if (old.key == map.key) { throw std::runtime_error("A layer with this region/level already exists. Choose a different level name."); } } db.save_layer(map); });
    }
}

void MapPage::edit_marker(bool existing, std::optional<QPointF> point) {
    const auto map = current_layer(); if (!map) { return; }
    const auto values = selected(); if (existing && values.size() != 1) { return; }
    MapMarker marker;
    if (existing) { marker = values.first(); }
    else {
        marker.map = map->key; marker.id = QStringLiteral("user:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto position = point.value_or(QPointF(map->width / 2, map->height / 2)); marker.x = position.x(); marker.y = position.y();
    }
    QDialog dialog(this); dialog.setObjectName(QStringLiteral("markerDialog")); dialog.setWindowTitle(existing ? tr("Edit marker") : tr("Add marker"));
    auto* form = new QFormLayout(&dialog);
    auto* label = new QLineEdit(marker.label); label->setObjectName(QStringLiteral("markerLabel")); label->setMaxLength(120);
    auto* category = new QComboBox; category->setObjectName(QStringLiteral("markerCategory")); category->setEditable(true);
    for (const auto* code : {"common", "exquisite", "precious", "luxurious", "remarkable", "custom"}) {
        const auto value = QString::fromLatin1(code); category->addItem(category_text(value), value);
    }
    const int category_index = category->findData(marker.category); category->setCurrentIndex(category_index);
    if (category_index < 0) { category->setEditText(marker.category); } category->lineEdit()->setMaxLength(64);
    auto* x = new QDoubleSpinBox; x->setObjectName(QStringLiteral("markerX")); x->setDecimals(6); x->setRange(0, map->width); x->setValue(marker.x);
    auto* y = new QDoubleSpinBox; y->setObjectName(QStringLiteral("markerY")); y->setDecimals(6); y->setRange(0, map->height); y->setValue(marker.y);
    auto* notes = new QPlainTextEdit(marker.notes); notes->setObjectName(QStringLiteral("markerNotes")); notes->setMaximumHeight(100);
    form->addRow(tr("Marker name"), label); form->addRow(tr("Category"), category); form->addRow(tr("X (right)"), x); form->addRow(tr("Y (down)"), y); form->addRow(tr("Notes (up to 2000 characters)"), notes);
    auto* buttons = dialog_buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        marker.label = label->text().trimmed();
        marker.category = category->currentIndex() >= 0 && category->currentText() == category->itemText(category->currentIndex()) ? category->currentData().toString() : category->currentText().trimmed();
        marker.x = x->value(); marker.y = y->value(); marker.notes = notes->toPlainText(); marker.origin = QStringLiteral("manual");
        const auto profile = store_.active_profile().id; run_task([marker, profile](Store& db) { db.save_marker(profile, marker); });
    }
}

void MapPage::import_bytes(const QByteArray& bytes, bool csv) {
    if (busy_) { return; } set_busy(true);
    parser_.setFuture(QtConcurrent::run([bytes, csv] {
        try { return ParsedImport{csv ? decode_csv(bytes) : decode_json(bytes), {}}; }
        catch (const std::exception& ex) { return ParsedImport{{}, QString::fromUtf8(ex.what())}; }
    }));
}
void MapPage::import_file() {
    const auto path = QFileDialog::getOpenFileName(this, tr("Import local dataset"), {}, tr("Local dataset (*.json *.csv)")); if (path.isEmpty()) { return; }
    try { import_bytes(bounded_read(path, max_transfer_bytes), path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)); }
    catch (const std::exception& ex) { error(QString::fromUtf8(ex.what())); }
}
void MapPage::export_file() {
    const auto map = current_layer(); if (!map) { return; }
    auto path = QFileDialog::getSaveFileName(this, tr("Export active layer"), QStringLiteral("markers.json"), tr("JSON (*.json);;CSV (*.csv)")); if (path.isEmpty()) { return; }
    if (!path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive) && !path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) { path += QStringLiteral(".json"); }
    const auto profile = store_.active_profile().id;
    run_task([path, profile, key = map->key](Store& db) { const auto data = db.export_layer(profile, key); atomic_export(path, path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive) ? encode_csv(data) : encode_json(data)); });
}
void MapPage::attach_background() {
    const auto map = current_layer(); if (!map) { return; }
    const auto path = QFileDialog::getOpenFileName(this, tr("Choose a permitted PNG background"), {}, tr("PNG (*.png)")); if (path.isEmpty()) { return; }
    QDialog dialog(this); dialog.setWindowTitle(tr("Background source and rights")); auto* form = new QFormLayout(&dialog);
    auto* credit = new QLineEdit; auto* license = new QLineEdit; credit->setMaxLength(512); license->setMaxLength(512);
    form->addRow(plain(tr("Use only images you created or have permission to use. PNG: up to 16 MiB and 4096 × 4096. The image is stretched to the declared local bounds, not automatically geolocated.")));
    form->addRow(tr("Source / author"), credit); form->addRow(tr("License / permission"), license);
    auto* buttons = dialog_buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        const auto author = credit->text(); const auto rights = license->text();
        run_task([path, author, rights, key = map->key](Store& db) { db.set_background(key, bounded_read(path, 16 * 1024 * 1024), author, rights); }, false, true);
    }
}
void MapPage::backup_file() {
    const auto path = QFileDialog::getSaveFileName(this, tr("Save a new database backup"), QStringLiteral("teyvat-backup.sqlite"), tr("SQLite backup (*.sqlite)"));
    if (!path.isEmpty()) { run_task([path](Store& db) { db.backup(path); }); }
}
void MapPage::restore_file() {
    const auto path = QFileDialog::getOpenFileName(this, tr("Restore database backup"), {}, tr("Database backup (*.sqlite *.db *.bak)"));
    if (!path.isEmpty() && confirm(this, tr("Restore all local data?"), tr("This replaces every local profile, setting, marker and progress record with the backup. A snapshot of current data is saved first. Continue?"))) {
        run_task([path](Store& db) { db.restore(path); }, true);
    }
}

void MapPage::show_progress() {
    QDialog dialog(this); dialog.setWindowTitle(tr("Local progress and history")); dialog.resize(850, 620);
    auto* layout = new QVBoxLayout(&dialog);
    auto* regions = new QTableWidget(0, 3); regions->setHorizontalHeaderLabels({tr("Region"), tr("Confirmed"), tr("Total")}); regions->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); regions->setMaximumHeight(160); regions->setEditTriggers(QAbstractItemView::NoEditTriggers); layout->addWidget(regions);
    QHash<QString, QPair<int, int>> counts;
    const auto profile = store_.active_profile().id;
    for (const auto& layer : store_.layers()) {
        (void)counts[layer.key.source + QStringLiteral(" / ") + layer.key.region];
        for (const auto& marker : store_.markers(profile, layer.key)) {
            if (!marker.retired) { auto& count = counts[layer.key.source + QStringLiteral(" / ") + layer.key.region]; ++count.second; if (is_confirmed(marker.status)) { ++count.first; } }
        }
    }
    regions->setRowCount(static_cast<int>(counts.size())); int row = 0;
    for (auto i = counts.cbegin(); i != counts.cend(); ++i, ++row) { regions->setItem(row, 0, new QTableWidgetItem(i.key())); regions->setItem(row, 1, new QTableWidgetItem(QString::number(i->first))); regions->setItem(row, 2, new QTableWidgetItem(QString::number(i->second))); }
    layout->addWidget(plain(tr("History is shown in pages of 250 events. Undo creates a new event; earlier records are retained.")));
    auto* events = new QTableWidget(0, 5); events->setHorizontalHeaderLabels({tr("Marker name"), tr("Before"), tr("After"), tr("Origin"), tr("Time")}); events->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); events->setEditTriggers(QAbstractItemView::NoEditTriggers); layout->addWidget(events);
    auto* pager = new QHBoxLayout; auto* newer = new QPushButton(tr("Newer events")); auto* older = new QPushButton(tr("Older events")); pager->addWidget(newer); pager->addWidget(older); layout->addLayout(pager);
    QVector<qint64> cursors{0}; QVector<HistoryEvent> history;
    const auto populate = [&] {
        history = store_.history(profile, 250, cursors.last()); events->setRowCount(static_cast<int>(history.size()));
        for (int i = 0; i < history.size(); ++i) {
            const auto& event = history[i];
            const auto origin = event.origin == QStringLiteral("manual") ? tr("Manual") : event.origin == QStringLiteral("undo") ? tr("Undo")
                : event.origin == QStringLiteral("detection") ? tr("Offline detection") : tr("Imported");
            const QStringList values{event.marker, status_text(read_status(event.before)), status_text(read_status(event.after)), origin, QDateTime::fromString(event.time, Qt::ISODateWithMs).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))};
            for (int j = 0; j < values.size(); ++j) { events->setItem(i, j, new QTableWidgetItem(values[j])); }
        }
        newer->setEnabled(cursors.size() > 1);
        older->setEnabled(!history.isEmpty() && !store_.history(profile, 1, history.last().id).isEmpty());
    };
    connect(older, &QPushButton::clicked, &dialog, [&] { if (!history.isEmpty()) { cursors.append(history.last().id); populate(); } });
    connect(newer, &QPushButton::clicked, &dialog, [&] { if (cursors.size() > 1) { cursors.removeLast(); populate(); } });
    populate();
    auto* close = dialog_buttons(QDialogButtonBox::Close); connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(close); dialog.exec();
}

void MapPage::retranslate() {
    if (!ready_) { return; }
    updating_ = true;
    heading_->setText(tr("Your local map")); help_->setText(tr("Drag to pan · Scroll to zoom · Select a chest to review or confirm it."));
    for (const auto& caption : captions_) { caption.first->setText(tr(caption.second)); }
    findChild<QLabel*>(QStringLiteral("recordingNotice"))->setText(tr("Recording mode · Progress saves automatically · Live game tracking is still in development"));
    cancel_analysis_->setText(tr("Cancel analysis"));
    search_->setPlaceholderText(tr("Search name, notes or marker ID")); search_->setAccessibleName(tr("Search markers"));
    layer_->setAccessibleName(tr("Map layer")); category_->setAccessibleName(tr("Category filter"));
    retired_->setText(tr("Show archived"));
    const auto filter = status_filter_->currentData(); const auto status = new_status_->currentData();
    status_filter_->clear(); new_status_->clear(); status_filter_->addItem(tr("All statuses"), QString());
    for (const auto value : statuses) { status_filter_->addItem(status_text(value), stored_status(value)); new_status_->addItem(status_text(value), stored_status(value)); }
    status_filter_->setCurrentIndex(std::max(0, status_filter_->findData(filter)));
    new_status_->setCurrentIndex(status.isValid() ? std::max(0, new_status_->findData(status)) : 3);
    table_->setHorizontalHeaderLabels({tr("Marker name"), tr("Category"), tr("Status")});
    updating_ = false;
}
void MapPage::changeEvent(QEvent* event) { if (ready_ && event->type() == QEvent::LanguageChange) { retranslate(); reload(); } QWidget::changeEvent(event); }
} // namespace teyvat
