#pragma once
#include "teyvat/window_capture.hpp"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTimer>
#include <QWidget>

class QLabel;
class QPushButton;
namespace teyvat {
class CaptureRecorder final : public QWidget {
    Q_OBJECT
public:
    explicit CaptureRecorder(QString data_directory, QWidget* parent = nullptr);
    ~CaptureRecorder() override;
    void start_recording(const WindowSource& source);
    void finish(bool interrupted);
signals:
    void saved(QString directory);
    void failed(QString message);
protected:
    void changeEvent(QEvent*) override;
    void closeEvent(QCloseEvent*) override;
private:
    void begin();
    void sample();
    void manifest(const QString& state);
    void retranslate();
    void set_status(const char* source, bool show_count = false);
    QString data_directory_, directory_, error_;
    std::optional<WindowSource> source_;
    QVector<QRect> regions_;
    QJsonArray frames_;
    QElapsedTimer clock_;
    QTimer timer_;
    QLabel *instructions_ = nullptr, *steps_ = nullptr, *rights_ = nullptr, *status_ = nullptr;
    QPushButton *start_ = nullptr, *stop_ = nullptr, *folder_ = nullptr;
    const char* status_source_ = nullptr;
    bool status_shows_count_ = false;
    qint64 first_sample_ = -1;
    qint64 total_bytes_ = 0;
    int empty_samples_ = 0;
};
} // namespace teyvat
