#include "teyvat/capture_recorder.hpp"
#include <QBuffer>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace teyvat {
namespace {
qint64 write_new_png(const QString& path, const QImage& image, qint64 remaining) {
    if (QFileInfo::exists(path)) { throw std::runtime_error("Capture filename already exists."); }
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) { throw std::runtime_error("Cannot encode capture PNG."); }
    if (bytes.size() > remaining) { throw std::runtime_error("Local recording reached its 64 MiB limit."); }
    QSaveFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { throw std::runtime_error("Cannot save capture PNG."); }
    return bytes.size();
}
bool visible_pixels(const QImage& image) {
    for (int y = 0; y < image.height(); y += 8) { for (int x = 0; x < image.width(); x += 8) { if (qGray(image.pixel(x, y)) > 4) { return true; } } }
    return false;
}
}
CaptureRecorder::CaptureRecorder(QString data_directory, QWidget* parent)
    : QWidget(parent, Qt::Window), data_directory_(std::move(data_directory)) {
    setObjectName(QStringLiteral("captureRecorder")); resize(460, 520);
    auto* layout = new QVBoxLayout(this);
    instructions_ = new QLabel; instructions_->setWordWrap(true); layout->addWidget(instructions_);
    steps_ = new QLabel; steps_->setWordWrap(true); layout->addWidget(steps_);
    rights_ = new QLabel; rights_->setWordWrap(true); layout->addWidget(rights_);
    status_ = new QLabel; status_->setWordWrap(true); layout->addWidget(status_);
    start_ = new QPushButton; start_->setObjectName(QStringLiteral("startCalibrationCapture")); layout->addWidget(start_); connect(start_, &QPushButton::clicked, this, &CaptureRecorder::begin);
    stop_ = new QPushButton; layout->addWidget(stop_); connect(stop_, &QPushButton::clicked, this, [this] { finish(true); });
    folder_ = new QPushButton; layout->addWidget(folder_);
    connect(folder_, &QPushButton::clicked, this, [this] { if (!directory_.isEmpty()) { QDesktopServices::openUrl(QUrl::fromLocalFile(directory_)); } });
    connect(&timer_, &QTimer::timeout, this, &CaptureRecorder::sample); timer_.setInterval(500);
    status_source_ = QT_TR_NOOP("Nothing is recorded until you press Start.");
    retranslate();
}
CaptureRecorder::~CaptureRecorder() { finish(true); }
void CaptureRecorder::retranslate() {
    setWindowTitle(tr("Teyvat Companion — Prepare game data"));
    instructions_->setText(tr("Record 30 seconds to prepare the Mondstadt beta. Only the upper-left minimap area and a central scene area are saved locally. No account details are requested or uploaded."));
    steps_->setText(tr("Start the game in windowed or borderless mode. Press Start, return to the game, briefly open the world map, then close it and open one chest if available. Capturing pauses when another window is active."));
    rights_->setText(tr("This explicitly saves game-image crops for local analysis. HoYoverse approval has not been established. Check the preview files before sharing them."));
    start_->setText(tr("Start local recording"));
    stop_->setText(tr("Stop recording"));
    folder_->setText(tr("Open saved folder"));
    set_status(status_source_, status_shows_count_);
}
void CaptureRecorder::set_status(const char* source, bool show_count) {
    status_source_ = source;
    status_shows_count_ = show_count;
    const auto text = tr(source);
    status_->setText(show_count ? text.arg(frames_.size()).arg(error_) : text);
}
void CaptureRecorder::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && status_source_) { retranslate(); }
    QWidget::changeEvent(event);
}
void CaptureRecorder::begin() {
    const auto source = find_game_window();
    if (!source || source->client_size.width() < 640 || source->client_size.height() < 480) {
        set_status(QT_TR_NOOP("Open Genshin Impact in windowed or borderless mode, then try again.")); return;
    }
    start_recording(*source);
}
void CaptureRecorder::start_recording(const WindowSource& source) {
    if (timer_.isActive()) { return; }
    if (source.client_size.width() < 640 || source.client_size.height() < 480) { throw std::invalid_argument("Recording requires at least a 640x480 client."); }
    source_ = source;
    const auto size = source_->client_size;
    const int scene = std::min({512, size.width(), size.height()});
    regions_ = {QRect(0, 0, std::min(640, size.width()), std::min(640, size.height())), QRect((size.width() - scene) / 2, (size.height() - scene) / 2, scene, scene)};
    directory_.clear(); frames_ = {}; error_.clear(); first_sample_ = -1; total_bytes_ = 0; empty_samples_ = 0; clock_.start(); start_->setEnabled(false);
    set_status(QT_TR_NOOP("Waiting for the game. Return to its window to start recording.")); timer_.start(); hide();
}
void CaptureRecorder::sample() {
    try {
        if (clock_.elapsed() > 90000 || (first_sample_ >= 0 && clock_.elapsed() - first_sample_ >= 30000) || frames_.size() >= 60) { finish(false); return; }
        const auto captured = capture_regions(*source_, source_->client_size, regions_);
        if (captured.reason == CaptureReason::Changed) { error_ = tr("The game window or resolution changed."); finish(true); return; }
        if (captured.reason == CaptureReason::Unavailable) { error_ = tr("Windows could not capture the selected regions. No fallback was attempted."); finish(true); return; }
        if (captured.reason != CaptureReason::Ready) { return; }
        if (!visible_pixels(captured.images[0]) && !visible_pixels(captured.images[1])) {
            if (++empty_samples_ >= 3) { error_ = tr("Capture returned empty images. Try a stable windowed or borderless game scene."); finish(true); } return;
        }
        empty_samples_ = 0;
        const auto captured_at = clock_.elapsed();
        if (directory_.isEmpty()) {
            directory_ = QDir(data_directory_).filePath(QStringLiteral("captures/%1-%2").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")), QUuid::createUuid().toString(QUuid::WithoutBraces)));
            if (!QDir().mkpath(directory_)) { throw std::runtime_error("Cannot create the local capture directory."); }
            first_sample_ = captured_at;
        }
        const auto prefix = QStringLiteral("frame-%1").arg(frames_.size(), 3, 10, QLatin1Char('0'));
        const auto mini = prefix + QStringLiteral("-minimap.png"), scene = prefix + QStringLiteral("-scene.png");
        total_bytes_ += write_new_png(QDir(directory_).filePath(mini), captured.images[0], 64 * 1024 * 1024 - total_bytes_);
        total_bytes_ += write_new_png(QDir(directory_).filePath(scene), captured.images[1], 64 * 1024 * 1024 - total_bytes_);
        frames_.append(QJsonObject{{QStringLiteral("time_ms"), captured_at - first_sample_}, {QStringLiteral("minimap"), mini}, {QStringLiteral("scene"), scene}});
        manifest(QStringLiteral("recording"));
    } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); finish(true); }
}
void CaptureRecorder::manifest(const QString& state) {
    QJsonArray areas;
    for (const auto& r : regions_) { areas.append(QJsonObject{{QStringLiteral("x"), r.x()}, {QStringLiteral("y"), r.y()}, {QStringLiteral("width"), r.width()}, {QStringLiteral("height"), r.height()}}); }
    const QJsonObject root{{QStringLiteral("format"), QStringLiteral("teyvat-calibration-capture")}, {QStringLiteral("version"), 1},
        {QStringLiteral("state"), state}, {QStringLiteral("app_version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("provenance"), QStringLiteral("Explicit user-started local capture of two selected client regions")},
        {QStringLiteral("rights"), QStringLiteral("Private local analysis; upstream game assets retain their rights. Not redistribution permission or official approval.")},
        {QStringLiteral("capture_size"), QJsonObject{{QStringLiteral("width"), source_->client_size.width()}, {QStringLiteral("height"), source_->client_size.height()}}},
        {QStringLiteral("regions"), areas}, {QStringLiteral("frames"), frames_}, {QStringLiteral("error"), error_}};
    const auto bytes = QJsonDocument(root).toJson(); QSaveFile file(QDir(directory_).filePath(QStringLiteral("capture.json")));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { throw std::runtime_error("Cannot save capture manifest."); }
}
void CaptureRecorder::finish(bool interrupted) {
    if (!timer_.isActive()) { return; }
    timer_.stop(); start_->setEnabled(true);
    if (!directory_.isEmpty()) {
        try { manifest(interrupted ? QStringLiteral("interrupted") : QStringLiteral("complete")); }
        catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); }
        emit saved(directory_);
    }
    if (directory_.isEmpty()) {
        set_status(QT_TR_NOOP("No frames saved. The game was inactive or capture was unavailable."));
    } else {
        set_status(QT_TR_NOOP("Saved %1 frames locally. Open the saved folder to review them. %2"), true);
    }
    if (!error_.isEmpty()) { emit failed(error_); }
    else if (directory_.isEmpty()) { emit failed(status_->text()); }
}
void CaptureRecorder::closeEvent(QCloseEvent* event) { finish(true); event->accept(); }
} // namespace teyvat
