#pragma once
#include "teyvat/store.hpp"
#include <QSet>
#include <QWidget>
#include <memory>

class QPushButton;
namespace teyvat {
// The official site owns login and all authenticated requests. Only pin IDs and
// acknowledgements cross this boundary; credentials and account IDs never do.
class HoYoLabSync final : public QWidget {
    Q_OBJECT
public:
    explicit HoYoLabSync(Store& store, QWidget* parent = nullptr);
    ~HoYoLabSync() override;
    void open(int map_id);
    void use_map(int map_id);
    [[nodiscard]] QSet<QString> collected(const QString& source) const;
signals:
    void progress_changed(const QString& source, const QSet<QString>& ids);
    void status_changed(const QString& text);
protected:
    void changeEvent(QEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void closeEvent(QCloseEvent*) override;
private:
    void initialize();
    void poll();
    void status(const char* source);
    void retranslate();
    struct State;
    std::unique_ptr<State> state_;
    QPushButton* continue_ = nullptr;
};
} // namespace teyvat
