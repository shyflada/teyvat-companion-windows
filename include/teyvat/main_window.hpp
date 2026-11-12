#pragma once

#include "teyvat/store.hpp"

#include <QFont>
#include <QMainWindow>
#include <functional>
#include <utility>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QCloseEvent;

namespace teyvat {
class MapPage;
void apply_interface_language(const QString& language);
void change_interface_language(Store& store, const QString& language);

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Store& store, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    QLabel* text(const char* source, const char* style = "");
    QPushButton* button(const char* source, const char* object_name = "");
    QWidget* page();
    void refresh_profiles();
    void load_profile();
    void save_settings();
    void apply_settings();
    void retranslate();
    void guarded(const std::function<void()>& operation);
    void show_page(int index);
    void show_example();

    Store& store_;
    Settings settings_;
    Profile profile_;
    QFont base_font_;
    std::vector<std::pair<QLabel*, const char*>> labels_;
    std::vector<std::pair<QPushButton*, const char*>> buttons_;
    std::vector<QPushButton*> nav_;
    QStackedWidget* pages_ = nullptr;
    QComboBox* selector_ = nullptr;
    QComboBox* language_ = nullptr;
    QComboBox* theme_ = nullptr;
    QComboBox* scale_ = nullptr;
    QListWidget* profiles_ = nullptr;
    QLineEdit* new_name_ = nullptr;
    QLineEdit* rename_ = nullptr;
    QLabel* data_path_ = nullptr;
    bool ready_ = false;
    MapPage* map_page_ = nullptr;
};

} // namespace teyvat
