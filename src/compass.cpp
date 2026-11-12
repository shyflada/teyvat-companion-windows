#include "teyvat/compass.hpp"
#include "teyvat/main_window.hpp"
#include "teyvat/capture_recorder.hpp"
#include "teyvat/map_page.hpp"
#include "teyvat/hoyolab_sync.hpp"
#include "teyvat/version.hpp"
#include <QAction>
#include <QActionGroup>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFile>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileDialog>
#include <QEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QKeySequenceEdit>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineF>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <tuple>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace teyvat {
bool minimap_present(const QImage&, const LocalizationConfig&);
struct OfficialRuntime {
    OfficialMap map;
    std::unique_ptr<OfficialWorldLocator> locator;
    LocalizationConfig reference;
    TrackingSetup hud;
    bool anchored = false;
    int misses = 0;
    qint64 last_global_attempt = -10000, next_reference_retry = -1;
    qint64 last_floor_attempt = -10000;
    bool hint_attempted = false;
    bool plane_ambiguous = false;
    QString layer = QStringLiteral("surface");
};
namespace {
void overlay_styles(QWidget& widget, bool editing = false) {
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HWND>(widget.winId()); auto styles = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    styles |= WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    if (editing) { styles &= ~WS_EX_TRANSPARENT; } else { styles |= WS_EX_TRANSPARENT; }
    SetWindowLongPtrW(handle, GWL_EXSTYLE, styles);
    SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#else
    (void)widget; (void)editing;
#endif
}
void seed_official_cache(const QString& source, const QString& destination, std::stop_token stop) {
    if (!QFileInfo::exists(QDir(source).filePath(QStringLiteral("map-2/snapshot.json")))) { return; }
    QDirIterator it(source, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories); int count = 0;
    while (it.hasNext()) {
        if (stop.stop_requested()) { throw std::runtime_error("Preparation cancelled."); }
        const auto file = it.next(); const auto info = it.fileInfo();
        if (++count > 8192 || info.size() > 64 * 1024 * 1024) { throw std::runtime_error("Invalid bundled official cache."); }
        const auto path = QDir(destination).filePath(QDir(source).relativeFilePath(file));
        if (!QFileInfo::exists(path)) { QDir().mkpath(QFileInfo(path).absolutePath()); if (!QFile::copy(file, path)) { throw std::runtime_error("Cannot prepare the local official map cache."); } }
    }
}
TrackingSetup official_hud(const QString& directory) {
    QFile file(QDir(directory).filePath(QStringLiteral("hud.json")));
    if (!file.exists()) {
        const auto game = find_game_window();
        if (!game) { throw std::runtime_error(QCoreApplication::translate("teyvat::CompassPanel", "Launch Genshin Impact, then start navigation.").toStdString()); }
        const double scale = game->client_size.height() / 1440.;
        const auto rect = [scale](int x, int y, int w, int h) { return QRect(qRound(x * scale), qRound(y * scale), qRound(w * scale), qRound(h * scale)); };
        TrackingSetup hud; hud.capture_size = game->client_size; auto& c = hud.localization;
        c.roi = rect(84, 28, 282, 282); c.presence_roi = rect(337, 24, 66, 64); c.presence_minimum = .75;
        c.reference = QImage(128, 128, QImage::Format_RGB32); c.reference.fill(Qt::black); c.origin = {500000, 500000};
        c.method = LocalizationMethod::Orb; c.allow_rotation = true; c.scales = {.5, 4}; validate_localization(c); return hud;
    }
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) { throw std::runtime_error("HUD calibration could not be read."); }
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value("format") != QJsonValue("teyvat-hud") || root.value("version") != QJsonValue(1)) { throw std::runtime_error("Invalid HUD calibration."); }
    const auto rect = [&](const char* key) { const auto v = root.value(QLatin1String(key)).toArray();
        if (v.size() != 4 || std::any_of(v.cbegin(), v.cend(), [](const auto& n) { return !n.isDouble() || std::trunc(n.toDouble()) != n.toDouble(); })) { throw std::runtime_error("Invalid HUD rectangle."); }
        return QRect(v[0].toInt(), v[1].toInt(), v[2].toInt(), v[3].toInt());
    };
    const QSize size(root.value("width").toInt(), root.value("height").toInt());
    LocalizationConfig config; config.roi = rect("minimap"); config.presence_roi = rect("presence");
    QFile png(QDir(directory).filePath(QStringLiteral("presence.png")));
    if (!png.open(QIODevice::ReadOnly) || png.size() > 256 * 1024) { throw std::runtime_error("HUD presence image is missing or oversized."); }
    config.presence_template = decode_background(png.readAll());
    if (config.presence_template.isNull()) { throw std::runtime_error("Invalid HUD presence image."); }
    config.presence_minimum = .75; config.reference = QImage(128, 128, QImage::Format_RGB32); config.reference.fill(Qt::black);
    config.origin = {500000, 500000}; config.method = LocalizationMethod::Orb; config.allow_rotation = true; config.scales = {.5, 4};
    validate_localization(config);
    if (size.width() < 640 || size.height() < 480 || size.width() > 7680 || size.height() > 4320
        || !QRect(QPoint(), size).contains(config.roi.united(config.presence_roi))) { throw std::runtime_error("HUD calibration exceeds the game window."); }
    TrackingSetup hud; hud.capture_size = size; hud.localization = config;
    if (root.contains("opening")) {
        hud.opening_roi = rect("opening");
        const auto load = [&](const QString& name) {
            QFile image(QDir(directory).filePath(name));
            if (!image.open(QIODevice::ReadOnly) || image.size() > 256 * 1024) { throw std::runtime_error("Opening template is missing or oversized."); }
            return decode_background(image.readAll());
        };
        hud.closed = load(QStringLiteral("closed.png")); hud.opened = load(QStringLiteral("opened.png"));
        if (!QRect(QPoint(), size).contains(hud.opening_roi) || hud.opening_roi.intersects(config.roi.united(config.presence_roi))
            || hud.opening_roi.width() < 16 || hud.opening_roi.height() < 16 || hud.opening_roi.width() > 128 || hud.opening_roi.height() > 128
            || hud.closed.size() != hud.opening_roi.size() || hud.opened.size() != hud.opening_roi.size()) { throw std::runtime_error("Invalid experimental opening calibration."); }
    }
    return hud;
}
Dataset official_update(const OfficialMap& map, Store& store, const QString& profile) {
    auto data = map.dataset; QSet<QString> ids, layers;
    for (const auto& marker : data.markers) { ids.insert(marker.id); }
    for (const auto& layer : data.maps) { layers.insert(layer_identity(layer.key)); }
    for (const auto& layer : store.layers()) { if (layer.key.source == map.surface().source) {
        for (auto marker : store.markers(profile, layer.key)) { if (marker.origin == QStringLiteral("import") && !ids.contains(marker.id)) {
            if (!layers.contains(layer_identity(layer.key))) { data.maps.append(layer); layers.insert(layer_identity(layer.key)); }
            marker.retired = true; marker.status = CollectionStatus::NotCollected; data.markers.append(marker);
        } }
    } } validate_dataset(data); return data;
}
template<class T> T* localized(T* object, const char* source) {
    object->setProperty("translationSource", source);
    object->setText(QCoreApplication::translate("teyvat::CompassPanel", source));
    return object;
}
QPoint on_screen(QPoint position, QSize size) {
    auto* screen = QGuiApplication::screenAt(position + QPoint(size.width() / 2, size.height() / 2));
    if (!screen) { screen = QGuiApplication::primaryScreen(); }
    if (!screen) { return {}; }
    const auto bounds = screen->availableGeometry();
    return {std::clamp(position.x(), bounds.left(), std::max(bounds.left(), bounds.right() - size.width() + 1)),
        std::clamp(position.y(), bounds.top(), std::max(bounds.top(), bounds.bottom() - size.height() + 1))};
}
bool game_is_foreground() {
#ifdef Q_OS_WIN
    wchar_t title[256]{};
    GetWindowTextW(GetForegroundWindow(), title, 256);
    const auto text = QString::fromWCharArray(title);
    return text == QStringLiteral("Genshin Impact") || text == QStringLiteral("原神");
#else
    return false;
#endif
}
}

TargetBeacon::TargetBeacon() : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
    | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput) {
    setObjectName(QStringLiteral("targetBeacon")); setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating); setFocusPolicy(Qt::NoFocus); setFixedSize(184, 70);
}
void TargetBeacon::set_target(double bearing, std::optional<double> metres, const QRect& game, bool is_near) {
    bearing_ = std::remainder(bearing, 360.); metres_ = metres; near_ = is_near;
    move(qRound(game.center().x() + std::clamp(bearing_ / 55., -1., 1.) * game.width() * .36 - width() * .5),
        game.top() + qRound(game.height() * .25)); update();
}
void TargetBeacon::showEvent(QShowEvent* event) { QWidget::showEvent(event); overlay_styles(*this); }
void TargetBeacon::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(QColor("#e4be6d"), 2)); p.setBrush(QColor(16, 27, 36, 215));
    p.drawRoundedRect(QRectF(2, 2, width() - 4, height() - 4), 10, 10);
    const QString course = near_ ? tr("You are near a chest") : std::abs(bearing_) > 100 ? tr("Behind") : bearing_ < -55 ? tr("Left") : bearing_ > 55 ? tr("Right") : tr("Chest direction");
    p.setPen(QColor("#e4be6d")); p.drawText(QRect(8, 8, width() - 16, 24), Qt::AlignCenter, course);
    auto font = p.font(); font.setBold(true); font.setPointSize(13); p.setFont(font); p.setPen(QColor("#e7eef4"));
    p.drawText(QRect(8, 34, width() - 16, 26), Qt::AlignCenter, metres_ ? tr("≈ %1 m").arg(qRound(*metres_)) : tr("Target"));
}
CompassOverlay::CompassOverlay() : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
        | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput) {
    setObjectName(QStringLiteral("compassOverlay")); setWindowTitle(QStringLiteral("Teyvat Companion Compass"));
    setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setFocusPolicy(Qt::NoFocus);
    set_extent(128);
    turn_.setDuration(70);
    connect(&turn_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { drawn_bearing_ = value.toDouble(); update(); });
}
void CompassOverlay::set_guidance(const std::optional<Guidance>& guidance, bool position_known, const QString& target,
    std::optional<double> camera, bool require_camera, std::optional<double> metres, bool is_near) {
    auto bearing = guidance ? guidance->minimap_bearing : std::nullopt;
    if (require_camera) { bearing = bearing && camera ? std::optional<double>(std::remainder(*bearing - *camera, 360.)) : std::nullopt; }
    if (bearing == bearing_ && position_known == position_known_ && guidance.has_value() == has_target_ && require_camera == camera_required_ && metres == metres_ && is_near == near_) { return; }
    camera_required_ = require_camera; metres_ = metres; near_ = is_near;
    position_known_ = position_known; has_target_ = guidance.has_value();
    setToolTip(QStringLiteral("<qt>") + target.toHtmlEscaped() + QStringLiteral("</qt>"));
    turn_.stop();
    if (bearing) {
        const double from = bearing_ ? drawn_bearing_ : *bearing;
        turn_.setStartValue(from); turn_.setEndValue(from + std::remainder(*bearing - from, 360.0)); turn_.start();
    }
    bearing_ = bearing; update();
}
void CompassOverlay::set_extent(int pixels) {
    setFixedSize(std::clamp(pixels, 96, 160), std::clamp(pixels, 96, 160)); place(pos()); update();
}
void CompassOverlay::place(QPoint position) { move(on_screen(position, size())); }
void CompassOverlay::set_editing(bool editing) {
    if (editing_ == editing) { return; }
    const bool visible = isVisible(); editing_ = editing;
    setWindowFlag(Qt::WindowTransparentForInput, !editing);
    apply_native_styles(); if (visible) { show(); } update();
}
void CompassOverlay::apply_native_styles() {
    overlay_styles(*this, editing_);
}
void CompassOverlay::showEvent(QShowEvent* event) { QWidget::showEvent(event); apply_native_styles(); }
void CompassOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); const double s = width();
    p.setPen(QPen(editing_ ? QColor("#e7b85e") : QColor("#83d7ba"), editing_ ? 3 : 1.5));
    p.setBrush(QColor(16, 27, 36, 225)); p.drawEllipse(QRectF(3, 3, s - 6, s - 6));
    if (bearing_ && !editing_) {
        p.save(); p.translate(s * .5, s * .34); p.rotate(drawn_bearing_);
        p.setPen(QPen(QColor("#83d7ba"), 3)); p.drawLine(QPointF(0, s * .14), QPointF(0, -s * .13));
        p.setBrush(QColor("#83d7ba")); p.setPen(Qt::NoPen);
        p.drawPolygon(QPolygonF{QPointF(0, -s * .20), QPointF(-s * .085, -s * .07), QPointF(s * .085, -s * .07)}); p.restore();
    } else {
        p.setPen(QPen(QColor("#e4be6d"), 2)); p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(s * .35, s * .24, s * .30, s * .22), 3, 3);
        p.drawLine(QPointF(s * .35, s * .33), QPointF(s * .65, s * .33));
        p.drawLine(QPointF(s * .5, s * .30), QPointF(s * .5, s * .40));
    }
    auto font = p.font(); font.setPointSizeF(std::max(8.5, s / 12.8)); p.setFont(font); p.setPen(QColor("#e7eef4"));
    const QString label = editing_ ? tr("Drag to move") : bearing_ ? (metres_ ? tr("≈ %1 m").arg(qRound(*metres_)) : camera_required_ ? tr("Camera direction") : tr("Minimap direction"))
        : !position_known_ ? tr("Position unknown") : has_target_ && near_ ? tr("Near target") : has_target_ && camera_required_ ? tr("Camera direction unknown") : has_target_ ? tr("Near target") : tr("No targets");
    p.drawText(QRectF(10, s * .52, s - 20, s * .34), Qt::AlignCenter | Qt::TextWordWrap, label);
}
void CompassOverlay::mousePressEvent(QMouseEvent* event) {
    if (editing_ && event->button() == Qt::LeftButton) { dragging_ = true; drag_offset_ = event->globalPosition().toPoint() - pos(); event->accept(); }
    else { QWidget::mousePressEvent(event); }
}
void CompassOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ && editing_) { place(event->globalPosition().toPoint() - drag_offset_); event->accept(); }
    else { QWidget::mouseMoveEvent(event); }
}
void CompassOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && event->button() == Qt::LeftButton) { dragging_ = false; emit moved(pos()); event->accept(); }
    else { QWidget::mouseReleaseEvent(event); }
}
bool CompassOverlay::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    const auto* msg = static_cast<MSG*>(message);
    if (msg->message == WM_MOUSEACTIVATE) { *result = MA_NOACTIVATE; return true; }
    if (!editing_ && msg->message == WM_NCHITTEST) { *result = HTTRANSPARENT; return true; }
#endif
    return QWidget::nativeEvent(type, message, result);
}

CompassPanel::CompassPanel(Store& store) : store_(store),
    options_(QFileInfo(store.path()).absolutePath() + QStringLiteral("/compass.ini"), QSettings::IniFormat) {
    setObjectName(QStringLiteral("compassPanel")); setWindowTitle(QStringLiteral("Teyvat Companion · %1").arg(QLatin1String(application_version)));
    setStyleSheet(QStringLiteral(
        "QWidget{background:#15232d;color:#e7eef4;}"
        "QLabel{background:transparent;}"
        "QPushButton,QToolButton,QComboBox{background:#20323e;border:1px solid #3e5b6b;border-radius:6px;padding:7px;}"
        "QPushButton:hover,QToolButton:hover,QComboBox:hover{border-color:#83d7ba;}"
        "QPushButton:focus,QToolButton:focus,QComboBox:focus{border-color:#b8ebd9;}"
        "QPushButton#startNavigation{background:#83d7ba;color:#10252a;font-weight:600;}"
        "QPushButton#startNavigation:hover{background:#a8e5ce;}"
        "QToolButton#compassSettings{padding:0;}"
        "QToolButton#compassSettings::menu-indicator{image:none;}"
        "QWidget#navigationTargetActions{background:#1c2d39;border-radius:6px;}"
        "QLabel#compassNotice{color:#b0c5d3;}"
    ));
    options_.setFallbacksEnabled(false);
    writable_ = options_.status() == QSettings::NoError;
    enabled_ = options_.value(QStringLiteral("enabled"), true).toBool(); economy_ = options_.value(QStringLiteral("economy"), true).toBool();
    const double stored_near = options_.value(QStringLiteral("nearDistance"), 20).toDouble();
    near_distance_ = std::isfinite(stored_near) && stored_near >= 5 && stored_near <= 100 ? stored_near : 20;
    const auto stored_key = QKeySequence::fromString(options_.value(QStringLiteral("collectShortcut"), QStringLiteral("F8")).toString(), QKeySequence::PortableText);
    if (!stored_key.isEmpty()) { collect_shortcut_ = stored_key; }
    const auto selected_map_id = options_.value(QStringLiteral("officialMap"), 2).toInt();
    official_map_id_ = selected_map_id == 7 || selected_map_id == 9 || selected_map_id == 36 ? selected_map_id : 2;
    official_area_id_ = options_.value(QStringLiteral("officialArea"), 0).toInt();
    const int stored_size = options_.value(QStringLiteral("size"), 128).toInt(), stored_opacity = options_.value(QStringLiteral("opacity"), 75).toInt();
    extent_ = (stored_size == 96 || stored_size == 128 || stored_size == 160) ? stored_size : 128;
    opacity_ = (stored_opacity == 50 || stored_opacity == 75 || stored_opacity == 95) ? stored_opacity : 75;
    const auto* screen = QGuiApplication::primaryScreen(); const auto bounds = screen ? screen->availableGeometry() : QRect(0, 0, 800, 600);
    saved_position_ = options_.value(QStringLiteral("position"), QPoint(bounds.center().x() - extent_ / 2, bounds.top() + 60)).toPoint();
    writable_ = options_.status() == QSettings::NoError;
    overlay_.set_extent(extent_); overlay_.setWindowOpacity(opacity_ / 100.0); overlay_.place(saved_position_);
    auto* frame = new QVBoxLayout(this);
    frame->setContentsMargins(16, 14, 16, 14);
    frame->setSizeConstraint(QLayout::SetFixedSize);
    auto* body = new QWidget(this);
    body->setFixedWidth(320);
    frame->addWidget(body);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(9);
    auto* header = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("Teyvat Companion"));
    auto title_font = title->font(); title_font.setPointSize(13); title_font.setBold(true); title->setFont(title_font);
    header->addWidget(title, 1);
    settings_button_ = new QToolButton;
    settings_button_->setObjectName(QStringLiteral("compassSettings"));
    settings_button_->setText(QStringLiteral("\u2699\uFE0E"));
    auto settings_font = settings_button_->font(); settings_font.setPointSize(14); settings_button_->setFont(settings_font);
    settings_button_->setFixedSize(36, 34);
    settings_button_->setFocusPolicy(Qt::StrongFocus);
    settings_button_->setToolTip(tr("Settings")); settings_button_->setAccessibleName(tr("Settings"));
    header->addWidget(settings_button_);
    layout->addLayout(header);
    region_box_ = new QComboBox; region_box_->setObjectName(QStringLiteral("navigationRegion"));
    region_box_->setAccessibleName(tr("Region to explore"));
    for (const auto& [id, name] : {std::pair{0, "Auto · All Teyvat"}, std::pair{1, "Mondstadt"}, std::pair{1001, "Dragonspine and surroundings"},
        std::pair{2, "Liyue"}, std::pair{3, "Inazuma"}, std::pair{4, "Sumeru"}, std::pair{8, "Fontaine"}, std::pair{11, "Natlan"}, std::pair{13, "Nod-Krai"}, std::pair{16, "Snezhnaya"}}) { region_box_->addItem(tr(name), id); region_box_->setItemData(region_box_->count() - 1, name, Qt::UserRole + 1); }
    const int region_index = region_box_->findData(official_area_id_); region_box_->setCurrentIndex(region_index >= 0 ? region_index : 0); official_area_id_ = region_box_->currentData().toInt();
    auto* region = localized(new QLabel, "Region");
    region->setBuddy(region_box_);
    layout->addWidget(region);
    region_box_->setEnabled(official_map_id_ == 2); layout->addWidget(region_box_);
    connect(region_box_, &QComboBox::currentIndexChanged, this, [this] { official_area_id_ = region_box_->currentData().toInt(); save_options(); apply_region_filter(); show_tracking(); });
    start_tracking_ = localized(new QPushButton, "Start navigation"); start_tracking_->setObjectName(QStringLiteral("startNavigation")); start_tracking_->setMinimumHeight(40); layout->addWidget(start_tracking_);
    connect(start_tracking_, &QPushButton::clicked, this, &CompassPanel::toggle_tracking);
    status_ = new QLabel; status_->setObjectName(QStringLiteral("compassStatus")); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); layout->addWidget(status_);
    target_area_ = new QWidget;
    target_area_->setObjectName(QStringLiteral("navigationTargetActions"));
    auto* target_layout = new QVBoxLayout(target_area_);
    target_layout->setContentsMargins(9, 8, 9, 9);
    collect_hint_ = new QLabel(tr("%1: confirm collected").arg(collect_shortcut_.toString(QKeySequence::NativeText)));
    collect_hint_->setWordWrap(true);
    target_layout->addWidget(collect_hint_);
    auto* navigation_actions = new QHBoxLayout;
    auto* map = localized(new QPushButton, "On map"); map->setObjectName(QStringLiteral("showTargetMap")); navigation_actions->addWidget(map);
    auto* next = localized(new QPushButton, "Skip"); next->setObjectName(QStringLiteral("nextNavigationTarget")); navigation_actions->addWidget(next);
    target_layout->addLayout(navigation_actions);
    layout->addWidget(target_area_);
    target_area_->hide();
    connect(map, &QPushButton::clicked, this, &CompassPanel::show_target_map);
    notice_ = new QLabel; notice_->setObjectName(QStringLiteral("compassNotice")); notice_->setTextFormat(Qt::PlainText); notice_->setWordWrap(true); layout->addWidget(notice_);
    notice_->hide();
    hoyolab_ = std::make_unique<HoYoLabSync>(store_, this); hoyolab_->use_map(official_map_id_);
    connect(hoyolab_.get(), &HoYoLabSync::progress_changed, this, [this](const QString& source, const QSet<QString>& ids) {
        if (tracking_ && tracking_->setup().analysis.map.source == source) { tracking_->set_remote_collected(ids); show_tracking(); }
    });
    menu_ = new QMenu(this);
    menu_->setToolTipsVisible(true);
    settings_button_->setMenu(menu_);
    settings_button_->setPopupMode(QToolButton::InstantPopup);
    auto* language_menu = menu_->addMenu(QString());
    language_menu->setObjectName(QStringLiteral("settingsLanguage"));
    localized(language_menu->menuAction(), "Language");
    auto* languages = new QActionGroup(language_menu);
    const auto read_language = [this, languages] {
        try {
            const auto current = store_.settings(store_.active_profile().id).language;
            for (auto* action : languages->actions()) { if (action->data().toString() == current) { action->setChecked(true); break; } }
        } catch (const std::exception&) {
            localized(notice_, "Settings could not be read. Existing settings are preserved.");
        }
    };
    for (const auto& [code, name, object_name] : {
            std::tuple{QStringLiteral("en"), QStringLiteral("English"), QStringLiteral("languageEn")},
            std::tuple{QStringLiteral("uk"), QStringLiteral("Українська"), QStringLiteral("languageUk")},
            std::tuple{QStringLiteral("ru"), QStringLiteral("Русский"), QStringLiteral("languageRu")}}) {
        auto* action = language_menu->addAction(name);
        action->setObjectName(object_name);
        action->setData(code);
        action->setCheckable(true);
        languages->addAction(action);
        connect(action, &QAction::triggered, this, [this, code, read_language] {
            try {
                change_interface_language(store_, code);
            } catch (const std::exception&) {
                read_language();
                localized(notice_, "Settings could not be saved. Check the local data folder.");
            }
        });
    }
    read_language();
    connect(language_menu, &QMenu::aboutToShow, this, read_language);
    auto* connect_map = localized(menu_->addAction(QString()), "Connect HoYoLAB account"); connect_map->setObjectName(QStringLiteral("connectHoYoLAB"));
    connect(connect_map, &QAction::triggered, this, [this] { hoyolab_->open(official_map_id_); });
    connect(hoyolab_.get(), &HoYoLabSync::status_changed, connect_map, &QAction::setToolTip);
    auto* collection_options = localized(menu_->addAction(QString()), "Collection shortcut and near distance"); collection_options->setObjectName(QStringLiteral("collectionSettings"));
    connect(collection_options, &QAction::triggered, this, &CompassPanel::collection_settings);
    auto* compass_settings = menu_->addMenu(QString());
    localized(compass_settings->menuAction(), "Compass settings");
    enabled_action_ = localized(compass_settings->addAction(QString()), "Compass enabled"); enabled_action_->setObjectName(QStringLiteral("compassEnabled")); enabled_action_->setCheckable(true); enabled_action_->setChecked(enabled_);
    edit_action_ = localized(compass_settings->addAction(QString()), "Move compass"); edit_action_->setCheckable(true); edit_action_->setObjectName(QStringLiteral("moveCompass"));
    edit_action_->setEnabled(enabled_);
    auto* sizes = compass_settings->addMenu(QString()); localized(sizes->menuAction(), "Size"); auto* size_group = new QActionGroup(this);
    for (const int value : {96, 128, 160}) {
        auto* action = sizes->addAction(QStringLiteral("%1 px").arg(value)); action->setObjectName(QStringLiteral("size%1").arg(value)); action->setCheckable(true); action->setChecked(value == extent_); size_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, value] { extent_ = value; overlay_.set_extent(value); save_options(); });
    }
    auto* opacities = compass_settings->addMenu(QString()); localized(opacities->menuAction(), "Opacity"); auto* opacity_group = new QActionGroup(this);
    for (const int value : {50, 75, 95}) {
        auto* action = opacities->addAction(QStringLiteral("%1%").arg(value)); action->setObjectName(QStringLiteral("opacity%1").arg(value)); action->setCheckable(true); action->setChecked(value == opacity_); opacity_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, value] { opacity_ = value; overlay_.setWindowOpacity(value / 100.0); save_options(); });
    }
    auto* economy = localized(compass_settings->addAction(QString()), "Economy mode"); economy->setObjectName(QStringLiteral("economyMode")); economy->setCheckable(true); economy->setChecked(economy_);
    connect(economy, &QAction::toggled, this, [this](bool value) { economy_ = value; timer_.setInterval(official_ ? 60 : value ? 500 : 150); save_options(); });
    menu_->addSeparator();
    auto* maps = menu_->addMenu(QString()); localized(maps->menuAction(), "Official map"); auto* map_group = new QActionGroup(this);
    for (const auto& [id, name] : {std::pair{2, "Teyvat"}, std::pair{7, "Enkanomiya"}, std::pair{9, "The Chasm: Underground Mines"}, std::pair{36, "Ancient Sacred Mountain"}}) {
        auto* map_action = localized(maps->addAction(QString()), name); map_action->setCheckable(true); map_group->addAction(map_action); map_action->setChecked(id == official_map_id_);
        connect(map_action, &QAction::triggered, this, [this, id] {
            stop_tracking(); official_.reset(); loaded_setup_.reset(); official_map_id_ = id; save_options();
            hoyolab_->use_map(id);
            region_box_->setEnabled(id == 2);
            localized(notice_, "Map changed. Start navigation and open M on the selected map.");
        });
    }
    connect(localized(menu_->addAction(QString()), "Start / stop navigation"), &QAction::triggered, this, &CompassPanel::toggle_tracking);
    auto* overview = localized(menu_->addAction(QString()), "Show target on map");
    overview->setObjectName(QStringLiteral("showNavigationMap"));
    connect(overview, &QAction::triggered, this, &CompassPanel::show_target_map);
    auto* skip = localized(menu_->addAction(QString()), "Skip target · Ctrl+Alt+N"); skip->setObjectName(QStringLiteral("skipTarget"));
    connect(next, &QPushButton::clicked, skip, &QAction::trigger);
    connect(skip, &QAction::triggered, this, [this] { if (tracking_) { try { tracking_->skip(); show_tracking(); } catch (const std::exception& error) { stop_tracking(); notice_->setText(QString::fromUtf8(error.what())); } } });
    auto* undo = localized(menu_->addAction(QString()), "Undo last change · Ctrl+Alt+U"); undo->setObjectName(QStringLiteral("undoNavigation"));
    connect(undo, &QAction::triggered, this, [this] {
        if (tracking_) { try { tracking_->undo(); show_tracking(); } catch (const std::exception& error) { notice_->setText(QString::fromUtf8(error.what())); } }
    });
    auto* confirm = localized(menu_->addAction(QString()), "Confirm chest collected"); confirm->setObjectName(QStringLiteral("confirmChestCollected"));
    connect(confirm, &QAction::triggered, this, &CompassPanel::confirm_current_target);
    auto* tools_menu = menu_->addMenu(QString());
    localized(tools_menu->menuAction(), "Tools");
    connect(localized(tools_menu->addAction(QString()), "Advanced tools"), &QAction::triggered, this, &CompassPanel::show_tools);
    connect(localized(tools_menu->addAction(QString()), "Load navigation data"), &QAction::triggered, this, &CompassPanel::load_navigation_data);
    auto* test = localized(tools_menu->addAction(QString()), "Run automatic example"); test->setObjectName(QStringLiteral("automaticExample"));
    connect(test, &QAction::triggered, this, &CompassPanel::show_navigation_example);
    auto* prepare = localized(tools_menu->addAction(QString()), "Prepare game data"); prepare->setObjectName(QStringLiteral("prepareGameData"));
    connect(prepare, &QAction::triggered, this, &CompassPanel::prepare_game_data);
    connect(localized(tools_menu->addAction(QString()), "Test overlay clicks"), &QAction::triggered, this, &CompassPanel::show_test_window);
    menu_->addSeparator();
    connect(localized(menu_->addAction(QString()), "Open control window"), &QAction::triggered, this, &CompassPanel::present_panel);
    auto* hide_panel = localized(menu_->addAction(QString()), "Hide to tray"); hide_panel->setObjectName(QStringLiteral("hideToTray")); hide_panel->setEnabled(QSystemTrayIcon::isSystemTrayAvailable());
    connect(hide_panel, &QAction::triggered, this, &QWidget::hide);
    auto* exit = localized(menu_->addAction(QString()), "Exit"); exit->setObjectName(QStringLiteral("exitCompanion"));
    connect(exit, &QAction::triggered, qApp, &QApplication::quit);
    const auto icon = QApplication::windowIcon();
    tray_ = new QSystemTrayIcon(icon.isNull() ? style()->standardIcon(QStyle::SP_ComputerIcon) : icon, this);
    tray_->setToolTip(windowTitle()); tray_->setContextMenu(menu_);
    if (QSystemTrayIcon::isSystemTrayAvailable()) { tray_->show(); }
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) { if (reason == QSystemTrayIcon::Trigger) { present_panel(); } });
    connect(enabled_action_, &QAction::toggled, this, &CompassPanel::set_enabled);
    connect(edit_action_, &QAction::toggled, this, [this](bool edit) { overlay_.set_editing(edit); update_visibility(); });
    connect(&overlay_, &CompassOverlay::moved, this, [this](QPoint position) { saved_position_ = position; save_options(); });
    connect(&timer_, &QTimer::timeout, this, &CompassPanel::update_visibility); timer_.start(economy_ ? 500 : 150);
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HWND>(winId());
    hotkey_toggle_ = RegisterHotKey(handle, 0x5440, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O');
    hotkey_move_ = RegisterHotKey(handle, 0x5441, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M');
    hotkey_skip_ = RegisterHotKey(handle, 0x5442, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'N');
    hotkey_undo_ = RegisterHotKey(handle, 0x5443, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'U');
    (void)register_collect_shortcut(collect_shortcut_);
    if (!hotkey_toggle_ || !hotkey_move_ || !hotkey_skip_ || !hotkey_undo_ || !hotkey_collect_) { localized(notice_, "A shortcut is unavailable. Use the control window or tray menu."); }
#endif
    if (!writable_) { localized(notice_, "Settings could not be read. Existing settings are preserved."); }
    capture_clock_.start();
    connect(&matcher_, &QFutureWatcher<LiveEstimate>::finished, this, [this] {
        if (!running_ || pending_epoch_ != capture_epoch_) { return; }
        try {
            const auto result = matcher_.result();
            if (result.cancelled) { return; }
            if (!result.error.isEmpty()) { throw std::runtime_error(result.error.toStdString()); }
            if (result.prepared && result.official && result.setup) {
                if (store_.active_profile().id != preparing_profile_) { throw std::runtime_error("Profile changed during preparation. Restart navigation."); }
                const auto data = official_update(result.official->map, store_, preparing_profile_);
                const auto review = store_.review_import(data, preparing_profile_, false);
                store_.apply_import(data, preparing_profile_, false, review.token);
                official_ = result.official; camera_config_ = official_->reference;
                tracking_ = std::make_unique<TrackingSession>(store_, *result.setup); tracking_source_ = find_game_window();
                tracking_->set_near_distance(near_distance_);
                tracking_->set_remote_collected(hoyolab_->collected(tracking_->setup().analysis.map.source));
                apply_region_filter();
                capture_clock_.restart(); last_confirm_time_ = last_scene_sample_ = -1000; last_sample_ = -1000; last_camera_sample_ = -1000; last_reading_ = -1; timer_.setInterval(60);
                localized(notice_, "Finding position from the minimap. Open M briefly if recovery needs more terrain. Distances are horizontal; height is unknown.");
                if (!result.warning.isEmpty()) { notice_->setText(result.warning + QLatin1Char('\n') + notice_->text()); }
                show_tracking(); return;
            }
            if (!tracking_ || !tracking_source_ || !source_active(*tracking_source_)) { return; }
            if (official_ && !result.layer.isEmpty() && result.layer != tracking_->setup().analysis.map.layer) {
                tracking_ = std::make_unique<TrackingSession>(store_, make_official_tracking_setup(official_->map, official_->hud, result.layer));
                tracking_->set_near_distance(near_distance_); tracking_->set_remote_collected(hoyolab_->collected(tracking_->setup().analysis.map.source)); apply_region_filter();
            }
            if (result.reference) { camera_config_ = *result.reference; }
            auto anchor = result.anchor_position;
            if (!anchor && result.estimate.position && tracking_->view().last_position) {
                const QPointF position(result.estimate.position->x, result.estimate.position->y);
                if (QLineF(position, *tracking_->view().last_position).length() > 64) { anchor = position; }
            }
            if (anchor) {
                const bool planned = planned_target_ && planned_teleport_ && capture_clock_.elapsed() <= planned_until_ && tracking_->target() == planned_target_
                    && QLineF(*anchor, planned_teleport_->position).length() <= 64;
                tracking_->reanchor(*anchor, planned);
                if (planned || capture_clock_.elapsed() > planned_until_) { planned_target_.reset(); planned_teleport_.reset(); }
            }
            // A slow tile download can outlive its source frame. Its reference
            // and map acquisition remain useful preview/search evidence; its
            // live pose/opening crop must never update guidance or progress.
            if (capture_clock_.elapsed() - pending_time_ > 1500) {
                tracking_->pause(); camera_pose_ = {}; camera_.reset(); last_reading_ = -1; show_tracking(); return;
            }
            tracking_->observe(result.estimate, pending_opening_, pending_time_); last_reading_ = pending_time_; show_tracking();
            if (official_) {
                camera_pose_ = result.estimate;
                if (!camera_pose_.position) { camera_.reset(); }
                show_official_guidance();
            }
        }
        catch (const std::exception& error) { stop_tracking(); notice_->setText(QString::fromUtf8(error.what())); }
    });
    update_visibility();
}
CompassPanel::~CompassPanel() {
    stop_tracking(); matcher_.waitForFinished(); tracking_.reset(); navigation_world_.reset(); demo_store_.reset(); demo_directory_.reset(); recorder_.reset();
    timer_.stop(); overlay_.hide(); beacon_.hide(); tray_->hide();
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HWND>(winId());
    if (hotkey_toggle_) { UnregisterHotKey(handle, 0x5440); } if (hotkey_move_) { UnregisterHotKey(handle, 0x5441); }
    if (hotkey_skip_) { UnregisterHotKey(handle, 0x5442); } if (hotkey_undo_) { UnregisterHotKey(handle, 0x5443); }
    if (hotkey_collect_) { UnregisterHotKey(handle, collect_hotkey_id_); }
#endif
}
void CompassPanel::set_enabled(bool enabled) {
    enabled_ = enabled; const QSignalBlocker blocker(enabled_action_); enabled_action_->setChecked(enabled);
    edit_action_->setEnabled(enabled); if (!enabled) { edit_action_->setChecked(false); } update_visibility(); save_options();
}
void CompassPanel::save_options() {
    if (!writable_) { return; }
    options_.setValue(QStringLiteral("enabled"), enabled_); options_.setValue(QStringLiteral("economy"), economy_);
    options_.setValue(QStringLiteral("size"), extent_); options_.setValue(QStringLiteral("opacity"), opacity_); options_.setValue(QStringLiteral("position"), saved_position_); options_.sync();
    options_.setValue(QStringLiteral("officialMap"), official_map_id_); options_.sync();
    options_.setValue(QStringLiteral("officialArea"), official_area_id_);
    options_.setValue(QStringLiteral("nearDistance"), near_distance_);
    options_.setValue(QStringLiteral("collectShortcut"), collect_shortcut_.toString(QKeySequence::PortableText)); options_.sync();
    if (options_.status() != QSettings::NoError) { writable_ = false; localized(notice_, "Settings could not be saved. Check the local data folder."); }
}
void CompassPanel::update_visibility() {
    sample_tracking();
    bool preview = false;
#ifdef Q_OS_WIN
    preview = test_window_ && test_window_->isVisible() && GetForegroundWindow() == reinterpret_cast<HWND>(test_window_->winId());
#endif
    if (preview_active_ && !preview && !overlay_.editing()) { overlay_.place(saved_position_); }
    preview_active_ = preview;
    const bool tracking_active = tracking_source_ && source_active(*tracking_source_);
    const bool active = preview || game_is_foreground() || tracking_active; const bool visible = enabled_ && (active || overlay_.editing());
    if (visible != overlay_.isVisible()) { if (visible) { overlay_.show(); } else { overlay_.hide(); } }
    if (!visible || overlay_.editing()) { beacon_.hide(); }
    update_panel_status();
}
void CompassPanel::update_panel_status() {
    const QString status = !enabled_ ? tr("Compass off") : overlay_.editing() ? tr("Move mode: drag the compass. Press Ctrl+Alt+M to finish.")
        : running_ && !tracking_ ? tr("Preparing official map · First startup may take a few minutes") : tracking_ ? tracking_status()
        : preview_active_ ? tr("Example window active · Position unknown") : tr("Ready to play");
    if (status_->text() != status) { status_->setText(status); }
    target_area_->setVisible(running_ && tracking_ && tracking_->target().has_value());
    notice_->setVisible(!notice_->text().isEmpty());
}
void CompassPanel::present_panel() { showNormal(); raise(); activateWindow(); }
void CompassPanel::show_target_map() {
    if (!tracking_) { localized(notice_, "Start navigation first to see its calibrated map."); return; }
    const auto& setup = tracking_->setup(); const auto layer = setup.dataset.maps.first();
    QDialog dialog(this); dialog.setObjectName(QStringLiteral("targetMapPreview")); dialog.setWindowTitle(tr("Chest location")); dialog.resize(720, 700);
    auto* layout = new QVBoxLayout(&dialog);
    auto* caption = new QLabel(official_ ? tr("Official chest marker and last recognized position. Local map snapshot; elevation and obstacles are not shown.")
        : tr("Selected chest and last recognized position. Community locations are approximate; this view is a snapshot."));
    caption->setWordWrap(true); caption->setTextFormat(Qt::PlainText); layout->addWidget(caption);
    auto* canvas = new MapCanvas(&dialog); canvas->set_layer(layer, {});
    if (official_) {
        auto viewport = layer; viewport.width = camera_config_.reference.width(); viewport.height = camera_config_.reference.height();
        canvas->set_layer(viewport, camera_config_.reference); QVector<MapMarker> markers;
        for (auto marker : store_.markers(store_.active_profile().id, setup.analysis.map)) {
            marker.x -= camera_config_.origin.x(); marker.y -= camera_config_.origin.y();
            if (marker.x >= 0 && marker.y >= 0 && marker.x < viewport.width && marker.y < viewport.height) { markers.append(marker); }
        }
        canvas->set_markers(markers);
        if (const auto target = tracking_->target()) { canvas->select_ids({QString::fromStdString(target->id)}); }
        if (tracking_->view().last_position) { canvas->set_position(*tracking_->view().last_position - camera_config_.origin); }
        const auto teleports = suggested_teleports();
        if (!teleports.isEmpty()) {
            auto* hint = new QLabel(tr("Possible shortcuts. Choose an unlocked teleport; height, access and travel time are unverified.")); hint->setWordWrap(true); hint->setTextFormat(Qt::PlainText); layout->addWidget(hint);
        }
        for (const auto& waypoint : teleports) {
            auto* open = new QPushButton(tr("Teleport: %1").arg(waypoint.name)); layout->addWidget(open);
            connect(open, &QPushButton::clicked, &dialog, [this, waypoint] {
                planned_target_ = tracking_->target(); planned_teleport_ = waypoint; planned_until_ = capture_clock_.elapsed() + 120000;
                const auto url = QStringLiteral("https://act.hoyolab.com/ys/app/interactive-map/index.html?lang=en-us#/map/%1?shown_types=2,3,154&center=%2,%3&zoom=0")
                    .arg(official_->map.id).arg(waypoint.position.y() - official_coordinate_offset, 0, 'f', 3).arg(waypoint.position.x() - official_coordinate_offset, 0, 'f', 3);
                QDesktopServices::openUrl(QUrl(url));
            });
        }
        const auto floor = std::find_if(official_->map.floors.cbegin(), official_->map.floors.cend(), [&](const auto& f) { return f.layer == setup.analysis.map.layer; });
        if (floor != official_->map.floors.cend() && floor->entrance) {
            auto* entrance = new QPushButton(tr("Show official entrance to this underground area")); layout->addWidget(entrance);
            connect(entrance, &QPushButton::clicked, &dialog, [this, position = *floor->entrance] {
                QDesktopServices::openUrl(QUrl(QStringLiteral("https://act.hoyolab.com/ys/app/interactive-map/index.html?lang=en-us#/map/%1?center=%2,%3&zoom=0")
                    .arg(official_->map.id).arg(position.y() - official_coordinate_offset, 0, 'f', 3).arg(position.x() - official_coordinate_offset, 0, 'f', 3)));
            });
        }
        layout->addWidget(canvas, 1); auto* close = new QPushButton(tr("Close")); layout->addWidget(close);
        connect(close, &QPushButton::clicked, &dialog, &QDialog::accept); dialog.exec(); return;
    }
    // Reference pixels may cover a subregion or use non-square map units.
    if (layer.width <= 2048 && layer.height <= 2048) {
        QImage background(static_cast<int>(layer.width), static_cast<int>(layer.height), QImage::Format_RGB32);
        background.fill(QColor("#1b2b35")); QPainter painter(&background);
        painter.drawImage(QRectF(setup.localization.origin, QSizeF(setup.localization.reference.width() * setup.localization.units_per_pixel.width(), setup.localization.reference.height() * setup.localization.units_per_pixel.height())), setup.localization.reference);
        painter.end(); canvas->set_layer(layer, background);
    }
    canvas->set_markers(store_.markers(store_.active_profile().id, setup.analysis.map));
    if (const auto target = tracking_->target()) { canvas->select_ids({QString::fromStdString(target->id)}); }
    canvas->set_position(tracking_->view().last_position); layout->addWidget(canvas, 1);
    auto* close = new QPushButton(tr("Close")); layout->addWidget(close); connect(close, &QPushButton::clicked, &dialog, &QDialog::accept); dialog.exec();
}
void CompassPanel::load_navigation_data() {
    const auto path = QFileDialog::getOpenFileName(this, tr("Choose calibrated navigation data"), {}, tr("Navigation setup (*.json)"));
    if (path.isEmpty()) { return; }
    try {
        auto setup = load_tracking_setup(path);
        if (setup.synthetic) { localized(notice_, "Synthetic data is used only by the separate automatic example."); return; }
        stop_tracking(); official_.reset(); loaded_setup_ = std::move(setup);
        if (writable_) { options_.setValue(QStringLiteral("navigationSetup"), QFileInfo(path).absoluteFilePath()); options_.sync(); }
        localized(notice_, "Navigation data loaded. Press Start navigation, then return to the game.");
    } catch (const std::exception& error) { notice_->setProperty("translationSource", QVariant()); notice_->setText(QString::fromUtf8(error.what())); }
}
void CompassPanel::toggle_tracking() {
    if (running_) { stop_tracking(); return; }
    if (matcher_.isRunning()) { localized(notice_, "Finishing the previous operation. Try Start again in a moment."); return; }
    try {
        if (!loaded_setup_) {
            start_official(); return;
        }
        if (loaded_setup_->synthetic) { loaded_setup_.reset(); localized(notice_, "Synthetic data is used only by the separate automatic example."); return; }
        stop_tracking(); tracking_.reset(); navigation_world_.reset(); demo_store_.reset(); demo_directory_.reset();
        const auto profile = store_.active_profile().id;
        const auto review = store_.review_import(loaded_setup_->dataset, profile, false);
        if (review.updated_markers > 0 || review.progress_changes > 0) {
            QMessageBox box(QMessageBox::Question, tr("Update navigation data"),
                tr("Update %1 existing marker definitions? Local collected statuses are preserved.").arg(review.updated_markers), QMessageBox::Yes | QMessageBox::No, this);
            box.setTextFormat(Qt::PlainText); box.setDefaultButton(QMessageBox::No); if (box.exec() != QMessageBox::Yes) { return; }
        }
        store_.apply_import(loaded_setup_->dataset, profile, false, review.token);
        tracking_ = std::make_unique<TrackingSession>(store_, *loaded_setup_); tracking_source_ = find_game_window();
        tracking_->set_near_distance(near_distance_);
        capture_clock_.restart(); last_confirm_time_ = last_scene_sample_ = -1000; last_sample_ = -1000; last_reading_ = -1; ++capture_epoch_; running_ = true;
        localized(start_tracking_, "Stop navigation"); localized(notice_, "Capturing only calibrated regions while the game is active. Tracking frames are not saved.");
        show_tracking();
    } catch (const std::exception& error) { stop_tracking(); notice_->setProperty("translationSource", QVariant()); notice_->setText(QString::fromUtf8(error.what())); }
}
void CompassPanel::stop_tracking() {
    if (running_) { notice_->setProperty("translationSource", QVariant()); notice_->clear(); }
    running_ = false; recovery_pending_ = true; ++capture_epoch_; match_stop_.request_stop();
    planned_target_.reset(); planned_teleport_.reset(); planned_until_ = -1;
    if (tracking_) { tracking_->pause(); }
    overlay_.set_guidance({}, false); overlay_.place(saved_position_); beacon_.hide(); camera_.reset(); camera_pose_ = {};
    if (start_tracking_) { localized(start_tracking_, "Start navigation"); }
    update_panel_status();
}
void CompassPanel::sample_tracking() {
    if (!running_ || !tracking_) { return; }
    if (official_) { sample_official(); return; }
    try {
        if (tracking_source_ && !inspect_window(tracking_source_->handle)) { tracking_source_.reset(); }
        if (!tracking_source_ && !tracking_->setup().synthetic) { tracking_source_ = find_game_window(); }
        if (!tracking_source_ || !source_active(*tracking_source_)) {
            ++capture_epoch_; match_stop_.request_stop(); tracking_->pause(); overlay_.set_guidance({}, false); return;
        }
        const auto time = capture_clock_.elapsed();
        if (last_reading_ >= 0 && time - last_reading_ > 1500) { ++capture_epoch_; match_stop_.request_stop(); tracking_->pause(); overlay_.set_guidance({}, false); last_reading_ = -1; }
        if (matcher_.isRunning() || time - last_sample_ < 240) { return; }
        auto config = tracking_->setup().localization;
        const auto source_region = config.presence_template.isNull() ? config.roi : config.roi.united(config.presence_roi);
        QVector<QRect> regions{source_region};
        if (!tracking_->setup().opening_roi.isEmpty()) { regions.append(tracking_->setup().opening_roi); }
        const auto captured = capture_regions(*tracking_source_, tracking_->setup().capture_size, regions, static_cast<quintptr>(overlay_.winId()));
        last_sample_ = time;
        if (captured.reason != CaptureReason::Ready) {
            ++capture_epoch_; tracking_->pause(); overlay_.set_guidance({}, false);
            if (captured.reason == CaptureReason::Changed) { localized(notice_, "Window size changed. Restore the calibrated resolution or load matching data."); }
            else if (captured.reason == CaptureReason::Occluded) { localized(notice_, "Keep the calibrated image regions visible and move the compass away from them."); }
            else if (captured.reason == CaptureReason::Unavailable) { localized(notice_, "Windows could not capture the regions. Navigation is paused."); }
            return;
        }
        pending_opening_ = captured.images.size() == 2 ? captured.images[1] : QImage{};
        pending_time_ = time; pending_epoch_ = capture_epoch_; match_stop_ = std::stop_source{};
        const auto minimap = captured.images[0]; config.roi.translate(-source_region.topLeft());
        if (!config.presence_template.isNull()) { config.presence_roi.translate(-source_region.topLeft()); }
        const auto stop = match_stop_.get_token();
        matcher_.setFuture(QtConcurrent::run([minimap, config, stop] {
            LiveEstimate result;
            try { result.estimate = localize_minimap(minimap, config, stop); }
            catch (const std::exception& error) { if (stop.stop_requested()) { result.cancelled = true; } else { result.error = QString::fromUtf8(error.what()); } }
            return result;
        }));
    } catch (const std::exception& error) { stop_tracking(); notice_->setProperty("translationSource", QVariant()); notice_->setText(QString::fromUtf8(error.what())); }
}
void CompassPanel::show_tracking() {
    if (!tracking_) { return; }
    const auto& view = tracking_->view(); const bool known = view.state != TrackingState::Paused && view.state != TrackingState::PositionUnknown && view.state != TrackingState::ProfileChanged;
    if (official_) { show_official_guidance(); }
    else { overlay_.set_guidance(view.guidance, known, view.target_label, {}, false, {}, view.near_target); }
    if (!tracking_->settings_error().isEmpty()) { notice_->setProperty("translationSource", QVariant()); notice_->setText(tracking_->settings_error()); }
    if (view.state == TrackingState::ProfileChanged) { stop_tracking(); }
    else { update_panel_status(); }
}
void CompassPanel::start_official() {
    stop_tracking(); official_.reset(); tracking_.reset(); navigation_world_.reset(); demo_store_.reset(); demo_directory_.reset();
    const auto navigation = QCoreApplication::applicationDirPath() + QStringLiteral("/navigation");
    const auto hud = official_hud(navigation);
    const auto cache = QFileInfo(store_.path()).absolutePath() + QStringLiteral("/official-map");
    preparing_profile_ = store_.active_profile().id; running_ = true; ++capture_epoch_; pending_epoch_ = capture_epoch_; last_hud_presence_.reset();
    match_stop_ = std::stop_source{}; const auto stop = match_stop_.get_token();
    localized(start_tracking_, "Stop navigation"); localized(notice_, "Downloading public official map resources. Captured pixels and local progress are never uploaded.");
    const auto map_id = official_map_id_;
    matcher_.setFuture(QtConcurrent::run([hud, navigation, cache, stop, map_id] {
        LiveEstimate result;
        try {
            seed_official_cache(QDir(navigation).filePath(QStringLiteral("official-map")), cache, stop);
            auto runtime = std::make_shared<OfficialRuntime>();
            try { runtime->map = refresh_official_map(cache, map_id, stop); }
            catch (const std::exception& error) {
                if (stop.stop_requested()) { throw; }
                runtime->map = load_official_map(cache, map_id); result.warning = QStringLiteral("Using the previous official cache: ") + QString::fromUtf8(error.what());
            }
            runtime->reference = hud.localization; runtime->hud = hud;
            runtime->locator = std::make_unique<OfficialWorldLocator>(runtime->map, stop);
            result.setup = make_official_tracking_setup(runtime->map, hud); result.official = std::move(runtime); result.prepared = true;
        } catch (const std::exception& error) { if (stop.stop_requested()) { result.cancelled = true; } else { result.error = QString::fromUtf8(error.what()); } }
        return result;
    }));
}
void CompassPanel::sample_official() {
    try {
        if (tracking_source_ && !inspect_window(tracking_source_->handle)) { tracking_source_.reset(); }
        if (!tracking_source_) { tracking_source_ = find_game_window(); }
        if (!tracking_source_ || !source_active(*tracking_source_)) {
            recovery_pending_ = true;
            ++capture_epoch_; match_stop_.request_stop(); tracking_->pause(); camera_pose_ = {}; camera_.reset();
            overlay_.set_guidance({}, false); beacon_.hide(); return;
        }
        const auto time = capture_clock_.elapsed(); if (time - last_camera_sample_ < 50) { return; } last_camera_sample_ = time;
        if (last_reading_ >= 0 && time - last_reading_ > 1500) {
            recovery_pending_ = true;
            ++capture_epoch_; match_stop_.request_stop(); tracking_->pause(); camera_pose_ = {}; camera_.reset(); last_reading_ = -1;
        }
        auto config = camera_config_; const auto source = config.roi.united(config.presence_roi);
        const auto captured = capture_regions(*tracking_source_, tracking_->setup().capture_size, {source}, static_cast<quintptr>(overlay_.winId()));
        if (captured.reason != CaptureReason::Ready) {
            recovery_pending_ = true;
            ++capture_epoch_; match_stop_.request_stop(); tracking_->pause(); camera_pose_ = {}; camera_.reset();
            overlay_.set_guidance({}, false); beacon_.hide();
            if (captured.reason == CaptureReason::Changed) { localized(notice_, "Window size changed. Restore the calibrated resolution or load matching data."); }
            return;
        }
        const auto minimap = captured.images.first(); config.roi.translate(-source.topLeft()); config.presence_roi.translate(-source.topLeft());
        if (config.presence_template.isNull()) {
            if (const auto learned = learn_hud_presence(minimap, config)) { config.presence_template = *learned; camera_config_.presence_template = *learned; }
        }
        const bool present = !config.presence_template.isNull() && minimap_present(minimap, config);
        if (last_hud_presence_ && *last_hud_presence_ != present) {
            recovery_pending_ = true;
            ++capture_epoch_; match_stop_.request_stop(); tracking_->pause(); camera_pose_ = {}; camera_.reset(); last_reading_ = -1; last_sample_ = -1000;
        }
        last_hud_presence_ = present;
        camera_ = present && camera_pose_.position && last_reading_ >= 0 && time - last_reading_ <= 1000
            ? estimate_camera_heading(minimap, config, camera_pose_) : std::nullopt;
        if (!present) { camera_pose_ = {}; tracking_->pause(); }
        show_official_guidance();
        if (present && tracking_->view().guidance && tracking_->view().guidance->map_units <= tracking_->setup().analysis.radius && time - last_scene_sample_ >= 180) {
            const auto size = tracking_->setup().capture_size; const int edge = std::clamp(qRound(size.height() * 512. / 1440), 256, std::min({640, size.width(), size.height()}));
            const QRect area((size.width() - edge) / 2, (size.height() - edge) / 2, edge, edge);
            const auto evidence = capture_regions(*tracking_source_, size, {area}, static_cast<quintptr>(overlay_.winId()));
            last_scene_sample_ = time;
            if (evidence.reason == CaptureReason::Ready) { tracking_->observe_scene(evidence.images.first(), time); }
        }
        if (matcher_.isRunning() || time - last_sample_ < (economy_ ? 500 : 250)) { return; }
        QImage world, opening;
        if (!present) {
            const auto size = tracking_->setup().capture_size; const int edge = std::min({512, size.width(), size.height()});
            const QRect scene((size.width() - edge) / 2, (size.height() - edge) / 2, edge, edge);
            const auto capture = capture_regions(*tracking_source_, size, {scene}, static_cast<quintptr>(overlay_.winId()));
            if (capture.reason != CaptureReason::Ready) { return; } world = capture.images.first();
        } else if (!tracking_->setup().opening_roi.isEmpty()) {
            const auto capture = capture_regions(*tracking_source_, tracking_->setup().capture_size, {tracking_->setup().opening_roi}, static_cast<quintptr>(overlay_.winId()));
            if (capture.reason != CaptureReason::Ready) { return; } opening = capture.images.first();
        }
        last_sample_ = time; pending_time_ = time; pending_epoch_ = capture_epoch_; pending_opening_ = opening;
        match_stop_ = std::stop_source{}; const auto stop = match_stop_.get_token(); const auto runtime = official_;
        const auto learned_presence = config.presence_template;
        const bool reacquire = present && recovery_pending_;
        const auto teleport = reacquire && time <= planned_until_ && planned_target_ && planned_target_ == tracking_->target()
            ? planned_teleport_ : std::nullopt;
        if (present) { recovery_pending_ = false; }
        const auto area = official_area_id_;
        const auto hint = tracking_->view().last_position;
        matcher_.setFuture(QtConcurrent::run([runtime, minimap, world, source, present, learned_presence, time, area, hint, reacquire, teleport, stop] {
            LiveEstimate out; bool ambiguous_plane = false;
            try {
                if (runtime->reference.presence_template.isNull() && !learned_presence.isNull()) { runtime->reference.presence_template = learned_presence; }
                if (!present) {
                    out.estimate.reason = LocalizationReason::MinimapAbsent;
                    const auto anchor = runtime->locator->locate(world, stop);
                    if (anchor.position) {
                        runtime->reference = official_local_reference(runtime->map, {anchor.position->x, anchor.position->y}, runtime->reference, stop);
                        runtime->anchored = true; runtime->misses = 0;
                        out.anchor_position = QPointF(anchor.position->x, anchor.position->y);
                        runtime->layer = QStringLiteral("surface");
                    } else if (hint && time - runtime->last_floor_attempt >= 3000) {
                        runtime->last_floor_attempt = time;
                        const auto found = match_official_planes(runtime->map, world, runtime->reference, *hint, runtime->layer, true, stop);
                        if (found.match && !found.ambiguous) {
                            runtime->reference = found.match->reference; runtime->layer = found.match->layer; runtime->anchored = true; runtime->misses = 0;
                            const auto& p = *found.match->estimate.position; out.anchor_position = QPointF(p.x, p.y);
                        }
                    }
                } else {
                    if (!runtime->anchored && !runtime->hint_attempted && hint) {
                        runtime->hint_attempted = true;
                        try { runtime->reference = official_local_reference(runtime->map, *hint, runtime->reference, stop); runtime->anchored = true; }
                        catch (const std::exception&) { if (stop.stop_requested()) { throw; } }
                    }
                    auto local = runtime->reference; local.roi.translate(-source.topLeft()); local.presence_roi.translate(-source.topLeft());
                    if (runtime->anchored) { out.estimate = localize_minimap(minimap, local, stop); }
                    if (runtime->layer != QStringLiteral("surface") && out.estimate.position) {
                        const auto floor = std::find_if(runtime->map.floors.cbegin(), runtime->map.floors.cend(), [&](const auto& f) { return f.layer == runtime->layer; });
                        if (floor == runtime->map.floors.cend() || !floor->bounds.contains({out.estimate.position->x, out.estimate.position->y})) { out.estimate = {}; }
                    }
                    if (!out.estimate.position && teleport) {
                        if (const auto arrival = verify_official_waypoint(runtime->map, minimap, local, *teleport, stop)) {
                            runtime->reference = arrival->reference;
                            runtime->reference.roi.translate(source.topLeft());
                            runtime->reference.presence_roi.translate(source.topLeft());
                            runtime->layer = arrival->layer;
                            runtime->anchored = true;
                            runtime->plane_ambiguous = false;
                            runtime->last_floor_attempt = time;
                            out.estimate = arrival->estimate;
                            const auto& p = *out.estimate.position;
                            out.anchor_position = QPointF(p.x, p.y);
                        }
                    }
                    const auto plane_hint = out.estimate.position ? std::optional<QPointF>(QPointF(out.estimate.position->x, out.estimate.position->y)) : hint;
                    if (plane_hint && time - runtime->last_floor_attempt >= 3000) {
                        runtime->last_floor_attempt = time;
                        const bool download = !out.estimate.position || out.estimate.limited_support || runtime->layer != QStringLiteral("surface");
                        const auto found = match_official_planes(runtime->map, minimap, local, *plane_hint, runtime->layer, false, stop, download);
                        if (found.ambiguous) { runtime->plane_ambiguous = true; ambiguous_plane = true; }
                        else if (found.match) {
                            runtime->plane_ambiguous = false;
                            if (!out.estimate.position || found.match->layer != runtime->layer) {
                                runtime->reference = found.match->reference;
                                runtime->reference.roi.translate(source.topLeft()); runtime->reference.presence_roi.translate(source.topLeft());
                                runtime->layer = found.match->layer; runtime->anchored = true; out.estimate = found.match->estimate;
                            }
                        }
                    }
                    if (runtime->plane_ambiguous) { out.estimate = {}; out.estimate.reason = LocalizationReason::Ambiguous; }
                    if (out.estimate.position) {
                        runtime->misses = 0; const auto& p = *out.estimate.position;
                        const auto center = QPointF(p.x, p.y) - runtime->reference.origin;
                        if (time >= runtime->next_reference_retry && (center.x() < 220 || center.y() < 220 || center.x() > 804 || center.y() > 804)) {
                            try {
                                if (runtime->layer == QStringLiteral("surface")) { runtime->reference = official_local_reference(runtime->map, {p.x, p.y}, runtime->reference, stop); }
                                else if (const auto ref = official_floor_reference(runtime->map, runtime->layer, {p.x, p.y}, runtime->reference, stop)) { runtime->reference = *ref; }
                            }
                            catch (const std::exception&) { if (stop.stop_requested()) { throw; } runtime->next_reference_retry = time + 3000; }
                        }
                    } else {
                        ++runtime->misses;
                        // A transient miss must not permanently disable local matching.
                        // Global proposals are verified against fresh detailed terrain.
                        // After HUD/focus loss, verify a new area on the first miss.
                        if (global_recovery_due(runtime->anchored, runtime->misses, time - runtime->last_global_attempt, reacquire, ambiguous_plane)) {
                            runtime->last_global_attempt = time;
                            try {
                                const auto preferred = runtime->map.id == 2 && area != 0 ? official_region_bounds(runtime->map, area) : QRectF{};
                                auto acquired = runtime->locator->locate_minimap(minimap, local, stop, preferred);
                                if (acquired && runtime->plane_ambiguous) {
                                    const auto& p = *acquired->estimate.position;
                                    const auto planes = match_official_planes(runtime->map, minimap, acquired->reference, {p.x, p.y}, runtime->layer, false, stop, false);
                                    if (planes.ambiguous || !planes.match) { acquired.reset(); }
                                    else { acquired = planes.match; runtime->plane_ambiguous = false; }
                                }
                                if (acquired) {
                                    runtime->reference = acquired->reference;
                                    runtime->reference.roi.translate(source.topLeft()); runtime->reference.presence_roi.translate(source.topLeft());
                                    runtime->anchored = true; runtime->misses = 0; out.estimate = acquired->estimate;
                                    runtime->layer = acquired->layer;
                                    const auto& p = *out.estimate.position; out.anchor_position = QPointF(p.x, p.y);
                                }
                            } catch (const std::exception&) { if (stop.stop_requested()) { throw; } }
                        }
                    }
                }
                out.reference = runtime->reference;
                out.layer = runtime->layer;
            } catch (const std::exception& error) { if (stop.stop_requested()) { out.cancelled = true; } else { out.error = QString::fromUtf8(error.what()); } }
            return out;
        }));
    } catch (const std::exception& error) { stop_tracking(); notice_->setText(QString::fromUtf8(error.what())); }
}
void CompassPanel::show_official_guidance() {
    if (!official_ || !tracking_) { beacon_.hide(); return; }
    const auto& view = tracking_->view(); const bool known = view.state != TrackingState::Paused && view.state != TrackingState::PositionUnknown && view.state != TrackingState::ProfileChanged;
    // Map 2's scale was checked against the in-game distance counter. Separate
    // maps have their own frames; do not silently assign that calibration.
    const auto metres = view.guidance && official_->map.id == 2 ? std::optional<double>(view.guidance->map_units) : std::nullopt;
    overlay_.set_guidance(view.guidance, known, view.target_label, camera_ ? std::optional<double>(camera_->degrees) : std::nullopt, true, metres, view.near_target);
    if (!running_ || !enabled_ || overlay_.editing() || !known || !view.guidance || (!view.near_target && (!view.guidance->minimap_bearing || !camera_))
        || !tracking_source_ || !source_active(*tracking_source_)) { beacon_.hide(); return; }
#ifdef Q_OS_WIN
    POINT origin{};
    if (!ClientToScreen(reinterpret_cast<HWND>(tracking_source_->handle), &origin)) { beacon_.hide(); return; }
    const double bearing = view.guidance->minimap_bearing && camera_ ? *view.guidance->minimap_bearing - camera_->degrees : 0;
    beacon_.set_target(bearing, metres, QRect(QPoint(origin.x, origin.y), tracking_source_->client_size), view.near_target); beacon_.show();
#endif
}
QString CompassPanel::tracking_status() const {
    if (!tracking_) { return tr("Position unknown"); }
    QString text;
    switch (tracking_->view().state) {
    case TrackingState::Paused: text = running_ ? tr("Waiting for the selected window") : tr("Navigation paused"); break;
    case TrackingState::PositionUnknown:
        text = official_ ? tr("Finding position automatically · M can help") : tracking_->view().position_reason == LocalizationReason::MinimapAbsent
            ? tr("Minimap hidden or HUD changed · Close the map/menu")
            : tr("Terrain not recognized · Return to the calibrated Mondstadt area"); break;
    case TrackingState::NoTarget: text = tr("No uncollected targets in this region and level"); break;
    case TrackingState::NearTarget: text = tr("You are near a chest · %1").arg(tracking_->view().target_label); break;
    case TrackingState::Guiding: text = official_ ? (!camera_ ? tr("Camera direction unknown · %1").arg(tracking_->view().target_label)
        : official_->map.id == 2 ? tr("Camera direction · %1 · ≈ %2 m").arg(tracking_->view().target_label).arg(qRound(tracking_->view().guidance->map_units)) : tr("Camera direction · %1").arg(tracking_->view().target_label))
        : tr("Direction on the minimap · %1").arg(tracking_->view().target_label); break;
    case TrackingState::Review: text = tracking_->view().review_reason == DetectionReason::AmbiguousMarkers
        ? tr("Several nearby chests · Opening not assigned") : tr("Opening uncertain · Progress preserved"); break;
    case TrackingState::ProfileChanged: text = tr("Profile changed · Restart navigation"); break;
    }
    if (tracking_->view().limited_position) { text = tr("Approximate position · %1").arg(text); }
    if (official_ && tracking_->view().guidance) {
        text += tr(" · Height unknown");
        if (tracking_->setup().analysis.map.layer != QStringLiteral("surface")) { text += tr(" · Level: %1").arg(tracking_->setup().dataset.maps.first().title); }
        if (!suggested_teleports().isEmpty()) { text += tr(" · Possible teleport · Open chest map"); }
    }
    if (tracking_->setup().synthetic) { text = tr("Synthetic example · %1").arg(text); }
    if (tracking_->view().automatic_changes > 0) { text += tr(" · Collected automatically: %1").arg(tracking_->view().automatic_changes); }
    if (tracking_->view().confirmed_changes > 0) { text += tr(" · Confirmed: %1").arg(tracking_->view().confirmed_changes); }
    return text;
}
void CompassPanel::apply_region_filter() {
    if (official_ && tracking_ && official_->map.id == 2) {
        tracking_->set_allowed_targets(official_area_id_ == 0 ? std::nullopt : std::optional<QSet<QString>>(official_region_markers(official_->map, official_area_id_)));
    }
}
QVector<OfficialWaypoint> CompassPanel::suggested_teleports() const {
    if (!official_ || !tracking_ || !tracking_->view().guidance || !tracking_->view().last_position) { return {}; }
    const auto target = tracking_->target(); if (!target) { return {}; }
    const auto marker = std::find_if(official_->map.dataset.markers.cbegin(), official_->map.dataset.markers.cend(), [&](const auto& m) { return m.id.toStdString() == target->id; });
    if (marker == official_->map.dataset.markers.cend()) { return {}; }
    return official_teleport_options(official_->map, marker->map.layer, *tracking_->view().last_position, {marker->x, marker->y});
}
void CompassPanel::show_navigation_example() {
    try {
        stop_tracking(); official_.reset(); tracking_.reset(); navigation_world_.reset(); demo_store_.reset(); demo_directory_.reset();
        const QDir example(QCoreApplication::applicationDirPath() + QStringLiteral("/examples/localization"));
        const auto setup = load_tracking_setup(example.filePath(QStringLiteral("session.json")));
        if (!setup.synthetic) { throw std::runtime_error("The bundled automatic example is not marked synthetic."); }
        demo_directory_ = std::make_unique<QTemporaryDir>(QFileInfo(store_.path()).absolutePath() + QStringLiteral("/navigation-example-XXXXXX"));
        if (!demo_directory_->isValid()) { throw std::runtime_error("Cannot create isolated example storage."); }
        demo_store_ = std::make_unique<Store>(demo_directory_->filePath(QStringLiteral("example.db")));
        const auto profile = demo_store_->active_profile().id;
        demo_store_->apply_import(setup.dataset, profile, false, demo_store_->review_import(setup.dataset, profile, false).token);
        tracking_ = std::make_unique<TrackingSession>(*demo_store_, setup);
        tracking_->set_near_distance(near_distance_);
        navigation_world_ = std::make_unique<QLabel>(this, Qt::Window);
        navigation_world_->setObjectName(QStringLiteral("navigationExampleWorld")); navigation_world_->setWindowTitle(tr("Synthetic navigation example — not Genshin Impact"));
        navigation_world_->setFixedSize(setup.capture_size); navigation_world_->setStyleSheet(QStringLiteral("QLabel{border:0;padding:0;margin:0;}"));
        if (const auto* screen = QGuiApplication::primaryScreen()) { navigation_world_->move(screen->availableGeometry().center() - QPoint(setup.capture_size.width() / 2, setup.capture_size.height() / 2)); }
        QVector<QPixmap> frames;
        for (int i = 0; i < 10; ++i) { QPixmap frame(example.filePath(QStringLiteral("frame-%1.png").arg(i, 3, 10, QLatin1Char('0')))); if (frame.isNull() || frame.size() != setup.capture_size) { throw std::runtime_error("Automatic example frames are missing."); } frames.append(frame); }
        navigation_world_->setPixmap(frames.first());
        auto* animation = new QTimer(navigation_world_.get()); animation->setInterval(1200);
        connect(animation, &QTimer::timeout, navigation_world_.get(), [this, animation, frames, index = 0]() mutable {
            if (++index >= frames.size()) { animation->stop(); return; } navigation_world_->setPixmap(frames[index]);
        });
        navigation_world_->show(); navigation_world_->raise(); navigation_world_->activateWindow();
        tracking_source_ = inspect_window(static_cast<quintptr>(navigation_world_->winId()));
        capture_clock_.restart(); last_confirm_time_ = last_scene_sample_ = -1000; last_sample_ = -1000; last_reading_ = -1; ++capture_epoch_; running_ = true; set_enabled(true);
        overlay_.place(navigation_world_->frameGeometry().topRight() + QPoint(20, 20));
        localized(start_tracking_, "Stop navigation"); localized(notice_, "Synthetic example. Watch two automatic collections; personal progress is unchanged."); animation->start();
    } catch (const std::exception& error) { stop_tracking(); notice_->setProperty("translationSource", QVariant()); notice_->setText(QString::fromUtf8(error.what())); }
}
void CompassPanel::prepare_game_data() {
    stop_tracking();
    if (!recorder_) {
        recorder_ = std::make_unique<CaptureRecorder>(QFileInfo(store_.path()).absolutePath(), this);
        connect(recorder_.get(), &CaptureRecorder::saved, this, [this](const QString& directory) {
            notice_->setProperty("translationSource", QVariant()); notice_->setText(tr("Game crops saved locally: %1").arg(QDir::toNativeSeparators(directory)));
            tray_->showMessage(QStringLiteral("Teyvat Companion"), tr("Local game recording saved. Open Prepare game data to review the folder."));
        });
        connect(recorder_.get(), &CaptureRecorder::failed, this, [this](const QString& message) {
            notice_->setProperty("translationSource", QVariant()); notice_->setText(message); tray_->showMessage(QStringLiteral("Teyvat Companion"), message);
        });
    }
    recorder_->showNormal(); recorder_->raise(); recorder_->activateWindow();
}
void CompassPanel::show_tools() {
    if (!tools_) { tools_ = std::make_unique<MainWindow>(store_); }
    tools_->showNormal(); tools_->raise(); tools_->activateWindow();
}
bool CompassPanel::register_collect_shortcut(const QKeySequence& shortcut) {
    if (shortcut.count() != 1 || shortcut.isEmpty()) { return false; }
    const auto combination = shortcut[0]; const auto key = combination.key(); const auto modifiers = combination.keyboardModifiers();
    if (key < Qt::Key_F1 && !(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) { return false; }
#ifdef Q_OS_WIN
    UINT vk = 0, mods = MOD_NOREPEAT;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) { vk = static_cast<UINT>(key); }
    else if (key >= Qt::Key_0 && key <= Qt::Key_9) { vk = static_cast<UINT>(key); }
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) { vk = VK_F1 + static_cast<UINT>(key - Qt::Key_F1); }
    else if (key == Qt::Key_Space) { vk = VK_SPACE; }
    if (!vk) { return false; }
    if (modifiers & Qt::ControlModifier) { mods |= MOD_CONTROL; } if (modifiers & Qt::AltModifier) { mods |= MOD_ALT; }
    if (modifiers & Qt::ShiftModifier) { mods |= MOD_SHIFT; } if (modifiers & Qt::MetaModifier) { mods |= MOD_WIN; }
    if (hotkey_collect_ && shortcut == collect_shortcut_) { return true; }
    const int id = hotkey_collect_ ? (collect_hotkey_id_ == 0x5444 ? 0x5445 : 0x5444) : collect_hotkey_id_;
    const auto handle = reinterpret_cast<HWND>(winId());
    if (!RegisterHotKey(handle, id, mods, vk)) { return false; }
    if (hotkey_collect_) { UnregisterHotKey(handle, collect_hotkey_id_); }
    hotkey_collect_ = true; collect_hotkey_id_ = id; collect_shortcut_ = shortcut; return true;
#else
    (void)shortcut; return false;
#endif
}
void CompassPanel::collection_settings() {
    QDialog dialog(this); dialog.setObjectName(QStringLiteral("collectionOptions")); dialog.setWindowTitle(tr("Collection settings"));
    auto* layout = new QFormLayout(&dialog); auto* key = new QKeySequenceEdit(collect_shortcut_); key->setObjectName(QStringLiteral("collectShortcut")); key->setMaximumSequenceLength(1);
    layout->addRow(tr("Confirm collected"), key);
    auto* distance = new QDoubleSpinBox; distance->setObjectName(QStringLiteral("nearDistance")); distance->setRange(5, 100); distance->setDecimals(0); distance->setValue(near_distance_);
    distance->setSuffix(official_map_id_ == 2 ? tr(" m") : tr(" map units")); layout->addRow(tr("Notify when within"), distance);
    auto* hint = new QLabel(tr("Use F1–F24 or a combination with Ctrl/Alt. The shortcut works while the game is active.")); hint->setWordWrap(true); layout->addRow(hint);
    auto* error = new QLabel; error->setWordWrap(true); layout->addRow(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel); layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto requested = key->keySequence();
        if (requested != collect_shortcut_ && !register_collect_shortcut(requested)) { error->setText(tr("This shortcut is unavailable. Choose another combination; the previous one is preserved.")); return; }
        near_distance_ = distance->value(); if (tracking_) { tracking_->set_near_distance(near_distance_); show_tracking(); }
        collect_hint_->setText(tr("%1: confirm collected").arg(collect_shortcut_.toString(QKeySequence::NativeText))); save_options(); dialog.accept();
    }); dialog.exec();
}
void CompassPanel::confirm_current_target() {
    if (!running_ || !tracking_) { localized(notice_, "Start navigation before confirming a chest."); return; }
    const auto now = capture_clock_.elapsed(); if (now - last_confirm_time_ < 500) { return; }
    try {
        const auto key = tracking_->confirm_target();
        if (!key) { localized(notice_, "No current chest to confirm."); return; }
        last_confirm_time_ = now; ++capture_epoch_; match_stop_.request_stop(); pending_opening_ = {};
        show_tracking(); localized(notice_, "Chest saved locally. The next nearest target is selected.");
    } catch (const std::exception& error) { notice_->setText(QString::fromUtf8(error.what())); }
}
void CompassPanel::show_test_window() {
    stop_tracking();
    edit_action_->setChecked(false);
    if (!test_window_) {
        test_window_ = std::make_unique<QWidget>(this, Qt::Window); test_window_->setObjectName(QStringLiteral("overlayTestWindow")); test_window_->setWindowTitle(QStringLiteral("Teyvat Companion — Overlay Test")); test_window_->resize(650, 450);
        auto* layout = new QVBoxLayout(test_window_.get()); auto* text = localized(new QLabel, "The compass should not steal focus. Click the button through the compass in normal mode."); text->setWordWrap(true); layout->addWidget(text);
        auto* button = localized(new QPushButton, "Click through the compass"); button->setObjectName(QStringLiteral("overlayTestButton")); button->setMinimumSize(300, 160); layout->addWidget(button, 1);
        auto* count = new QLabel(tr("Clicks: 0")); count->setObjectName(QStringLiteral("overlayClickCount")); layout->addWidget(count);
        count->setProperty("clicks", 0);
        connect(button, &QPushButton::clicked, test_window_.get(), [count] { const auto clicks = count->property("clicks").toInt() + 1; count->setProperty("clicks", clicks); count->setText(tr("Clicks: %1").arg(clicks)); });
    }
    test_window_->showNormal(); test_window_->raise(); test_window_->activateWindow();
    auto* button = test_window_->findChild<QPushButton*>(QStringLiteral("overlayTestButton")); test_window_->layout()->activate();
    overlay_.place(button->mapToGlobal(button->rect().center()) - QPoint(overlay_.width() / 2, overlay_.height() / 2));
    set_enabled(true); QTimer::singleShot(100, this, &CompassPanel::update_visibility);
}
void CompassPanel::closeEvent(QCloseEvent* event) {
    event->accept(); qApp->quit();
}
void CompassPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && status_) {
        for (auto* object : findChildren<QObject*>()) {
            const auto source = object->property("translationSource").toByteArray();
            if (source.isEmpty()) { continue; }
            const auto text = tr(source.constData());
            if (auto* label = qobject_cast<QLabel*>(object)) { label->setText(text); }
            else if (auto* control = qobject_cast<QAbstractButton*>(object)) { control->setText(text); }
            else if (auto* action = qobject_cast<QAction*>(object)) { action->setText(text); }
        }
        if (test_window_) {
            auto* count = test_window_->findChild<QLabel*>(QStringLiteral("overlayClickCount"));
            count->setText(tr("Clicks: %1").arg(count->property("clicks").toInt()));
        }
        collect_hint_->setText(tr("%1: confirm collected").arg(collect_shortcut_.toString(QKeySequence::NativeText)));
        for (int i = 0; i < region_box_->count(); ++i) { const auto key = region_box_->itemData(i, Qt::UserRole + 1).toByteArray(); region_box_->setItemText(i, tr(key.constData())); }
        region_box_->setAccessibleName(tr("Region to explore"));
        settings_button_->setToolTip(tr("Settings")); settings_button_->setAccessibleName(tr("Settings"));
        overlay_.update(); update_visibility();
    }
    QWidget::changeEvent(event);
}
bool CompassPanel::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    const auto* msg = static_cast<MSG*>(message);
    if (msg->message == WM_HOTKEY) {
        if (msg->wParam == 0x5440) { set_enabled(!enabled_); } else if (msg->wParam == 0x5441 && enabled_) { edit_action_->toggle(); }
        else if (msg->wParam == 0x5442 && tracking_) { menu_->findChild<QAction*>(QStringLiteral("skipTarget"))->trigger(); }
        else if (msg->wParam == 0x5443 && tracking_) { menu_->findChild<QAction*>(QStringLiteral("undoNavigation"))->trigger(); }
        else if (msg->wParam == static_cast<WPARAM>(collect_hotkey_id_) && hotkey_collect_ && running_ && tracking_source_ && source_active(*tracking_source_)) { confirm_current_target(); }
        *result = 0; return true;
    }
#endif
    return QWidget::nativeEvent(type, message, result);
}
}
