#include "teyvat/main_window.hpp"
#include "teyvat/map_page.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>
#include <stdexcept>

namespace teyvat {
namespace {

QFrame* card(QVBoxLayout* parent) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("card"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(12);
    parent->addWidget(frame);
    return frame;
}

QString current_interface_language() {
    const auto language = qApp->property("teyvatLanguage").toString();
    return language.isEmpty() ? QStringLiteral("en") : language;
}

void set_interface_language(const QString& language, const std::function<void()>& save = {}) {
    const bool changed = language != current_interface_language();
    std::unique_ptr<QTranslator> replacement;
    if (changed && language != QStringLiteral("en")) {
        replacement = std::make_unique<QTranslator>();
        if (!replacement->load(QStringLiteral("teyvat_") + language,
            QCoreApplication::applicationDirPath() + QStringLiteral("/translations"))) {
            throw std::runtime_error("The selected translation is missing. Re-extract the complete application folder.");
        }
    }
    if (save) { save(); }
    if (!changed) { return; }

    const auto name = QStringLiteral("teyvatTranslator");
    auto* previous = qApp->findChild<QTranslator*>(name, Qt::FindDirectChildrenOnly);
    qApp->setProperty("teyvatLanguage", language);
    if (replacement) {
        replacement->setObjectName(name);
        replacement->setParent(qApp);
        qApp->installTranslator(replacement.release());
    }
    if (previous) {
        qApp->removeTranslator(previous);
        delete previous;
    }
}

} // namespace

void apply_interface_language(const QString& language) {
    set_interface_language(language);
}

void change_interface_language(Store& store, const QString& language) {
    const auto profile_id = store.active_profile().id;
    auto settings = store.settings(profile_id);
    if (settings.language == language) {
        apply_interface_language(language);
        return;
    }
    settings.language = language;
    set_interface_language(language, [&] { store.save_settings(profile_id, settings); });
}

QLabel* MainWindow::text(const char* source, const char* style) {
    auto* label = new QLabel;
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setProperty("kind", QString::fromLatin1(style));
    labels_.emplace_back(label, source);
    return label;
}

QPushButton* MainWindow::button(const char* source, const char* object_name) {
    auto* result = new QPushButton;
    result->setObjectName(QString::fromLatin1(object_name));
    result->setCursor(Qt::PointingHandCursor);
    buttons_.emplace_back(result, source);
    return result;
}

QWidget* MainWindow::page() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(32, 28, 32, 28);
    layout->setSpacing(18);
    scroll->setWidget(content);
    pages_->addWidget(scroll);
    return content;
}

MainWindow::MainWindow(Store& store, QWidget* parent)
    : QMainWindow(parent), store_(store), base_font_(QApplication::font()) {
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("Teyvat Companion · Development Preview"));
    resize(1120, 800);
    setMinimumSize(900, 650);
    auto* root = new QWidget;
    auto* root_layout = new QHBoxLayout(root);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);
    setCentralWidget(root);

    auto* sidebar = new QFrame;
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(220);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(22, 30, 22, 24);
    side->setSpacing(10);
    auto* brand = new QLabel(QStringLiteral("✦  TEYVAT"));
    brand->setProperty("kind", QStringLiteral("brand"));
    side->addWidget(brand);
    auto* subbrand = new QLabel(QStringLiteral("C O M P A N I O N"));
    subbrand->setProperty("kind", QStringLiteral("muted"));
    side->addWidget(subbrand);
    side->addSpacing(30);
    for (const auto* name : {"Start", "Local profiles", "Settings", "Local map"}) {
        auto* nav = button(name);
        nav->setProperty("nav", true);
        nav->setCheckable(true);
        const int index = static_cast<int>(nav_.size());
        nav->setObjectName(QStringLiteral("nav%1").arg(index));
        connect(nav, &QPushButton::clicked, this, [this, index] { show_page(index); });
        nav_.push_back(nav);
        side->addWidget(nav);
    }
    side->addStretch();
    side->addWidget(text("On your device", "eyebrow"));
    side->addWidget(text("No account required for local profiles.", "muted"));
    side->addSpacing(14);
    side->addWidget(text("Development preview", "badge"));
    root_layout->addWidget(sidebar);

    auto* body = new QWidget;
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);
    auto* header = new QFrame;
    header->setObjectName(QStringLiteral("header"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(32, 16, 32, 16);
    header_layout->addWidget(text("YOUR EXPLORATION SPACE", "eyebrow"));
    header_layout->addStretch();
    auto* profile_label = text("Local profile");
    header_layout->addWidget(profile_label);
    selector_ = new QComboBox;
    selector_->setObjectName(QStringLiteral("profileSelector"));
    selector_->setMinimumWidth(190);
    profile_label->setBuddy(selector_);
    header_layout->addWidget(selector_);
    body_layout->addWidget(header);
    pages_ = new QStackedWidget;
    body_layout->addWidget(pages_);
    root_layout->addWidget(body, 1);

    auto* overview = page();
    auto* overview_layout = qobject_cast<QVBoxLayout*>(overview->layout());
    overview_layout->addWidget(text("Start with one click", "title"));
    overview_layout->addWidget(text("Try automatic chest tracking on a ready-made recording.", "muted"));
    auto* start_card = card(overview_layout);
    start_card->layout()->addWidget(text("See automation in action", "heading"));
    start_card->layout()->addWidget(text("The example finds a position and collects two test chests. Its data is temporary."));
    auto* example = button("Try example", "tryExample"); example->setProperty("primary", true);
    connect(example, &QPushButton::clicked, this, &MainWindow::show_example); start_card->layout()->addWidget(example);
    auto* my_map = button("Open my map", "openMyMap"); connect(my_map, &QPushButton::clicked, this, [this] { show_page(3); });
    start_card->layout()->addWidget(my_map);
    overview_layout->addWidget(text("Available now: prepared recordings. Live game tracking and official-map synchronization are still in development.", "muted"));

    auto* official = card(overview_layout);
    official->layout()->addWidget(text("OFFICIAL TEYVAT MAP", "eyebrow"));
    official->layout()->addWidget(text("Your map. Your browser.", "heading"));
    auto* login_note = text("Sign in on the official map website to save opened-chest marks there. Your browser handles sign-in; Teyvat Companion never asks for your password or reads cookies or tokens.");
    login_note->setObjectName(QStringLiteral("officialLoginNote"));
    official->layout()->addWidget(login_note);
    official->layout()->addWidget(text("Official-map synchronization is not available yet.", "muted"));
    auto* open = button("Open official map ↗", "openOfficialMap");
    connect(open, &QPushButton::clicked, this, [this] {
        if (!QDesktopServices::openUrl(QUrl(QStringLiteral("https://act.hoyolab.com/ys/app/interactive-map/index.html")))) {
            QMessageBox::warning(this, tr("Cannot open browser"), tr("Open this address in your browser: https://act.hoyolab.com/ys/app/interactive-map/index.html"));
        }
    });
    official->layout()->addWidget(open);
    overview_layout->addStretch();

    auto* profile_page = page();
    auto* profile_layout = qobject_cast<QVBoxLayout*>(profile_page->layout());
    profile_layout->addWidget(text("Local profiles", "title"));
    profile_layout->addWidget(text("Separate workspaces on this computer. These are not HoYoLAB accounts.", "muted"));
    auto* list_card = card(profile_layout);
    list_card->layout()->addWidget(text("Choose a profile", "heading"));
    profiles_ = new QListWidget;
    profiles_->setObjectName(QStringLiteral("profileList"));
    profiles_->setMinimumHeight(135);
    profiles_->setMaximumHeight(220);
    list_card->layout()->addWidget(profiles_);
    auto* activate = button("Use selected profile", "activateProfile");
    list_card->layout()->addWidget(activate);
    connect(activate, &QPushButton::clicked, this, [this] {
        guarded([&] {
            if (const auto* item = profiles_->currentItem()) {
                store_.activate_profile(item->data(Qt::UserRole).toString());
                load_profile();
            }
        });
    });
    auto* create_card = card(profile_layout);
    auto* create_label = text("New profile name", "heading");
    create_card->layout()->addWidget(create_label);
    new_name_ = new QLineEdit;
    new_name_->setObjectName(QStringLiteral("newProfileName"));
    new_name_->setMaxLength(80);
    create_label->setBuddy(new_name_);
    create_card->layout()->addWidget(new_name_);
    auto* create = button("Create profile", "createProfile");
    create->setProperty("primary", true);
    create_card->layout()->addWidget(create);
    create->setEnabled(false);
    connect(new_name_, &QLineEdit::textChanged, this, [create](const QString& value) { create->setEnabled(!value.trimmed().isEmpty()); });
    connect(create, &QPushButton::clicked, this, [this] {
        guarded([&] {
            (void)store_.create_profile(new_name_->text());
            new_name_->clear();
            load_profile();
            new_name_->setFocus();
        });
    });
    connect(new_name_, &QLineEdit::returnPressed, create, &QPushButton::click);
    auto* rename_card = card(profile_layout);
    auto* rename_label = text("Rename active profile", "heading");
    rename_card->layout()->addWidget(rename_label);
    rename_ = new QLineEdit;
    rename_->setObjectName(QStringLiteral("renameProfileName"));
    rename_->setMaxLength(80);
    rename_label->setBuddy(rename_);
    rename_card->layout()->addWidget(rename_);
    auto* rename_button = button("Save name", "renameProfile");
    rename_card->layout()->addWidget(rename_button);
    connect(rename_button, &QPushButton::clicked, this, [this] {
        guarded([&] { store_.rename_profile(profile_.id, rename_->text()); load_profile(); });
    });
    profile_layout->addStretch();

    auto* settings_page = page();
    auto* settings_layout = qobject_cast<QVBoxLayout*>(settings_page->layout());
    settings_layout->addWidget(text("Make yourself at home.", "title"));
    settings_layout->addWidget(text("Changes are saved automatically to the active local profile.", "muted"));
    auto* appearance = card(settings_layout);
    auto* language_label = text("Interface language", "heading");
    appearance->layout()->addWidget(language_label);
    language_ = new QComboBox;
    language_->setObjectName(QStringLiteral("languageSetting"));
    language_->addItem(QStringLiteral("English"), QStringLiteral("en"));
    language_->addItem(QStringLiteral("Українська"), QStringLiteral("uk"));
    language_->addItem(QStringLiteral("Русский"), QStringLiteral("ru"));
    language_label->setBuddy(language_);
    appearance->layout()->addWidget(language_);
    auto* theme_label = text("Theme", "heading");
    appearance->layout()->addWidget(theme_label);
    theme_ = new QComboBox;
    theme_->setObjectName(QStringLiteral("themeSetting"));
    theme_->addItem(QString(), QStringLiteral("dark"));
    theme_->addItem(QString(), QStringLiteral("light"));
    theme_label->setBuddy(theme_);
    appearance->layout()->addWidget(theme_);
    auto* scale_label = text("Interface size", "heading");
    appearance->layout()->addWidget(scale_label);
    scale_ = new QComboBox;
    scale_->setObjectName(QStringLiteral("scaleSetting"));
    for (const int scale : {100, 125, 150}) {
        scale_->addItem(QStringLiteral("%1%").arg(scale), scale);
    }
    scale_label->setBuddy(scale_);
    appearance->layout()->addWidget(scale_);
    auto* storage = card(settings_layout);
    storage->layout()->addWidget(text("Local data", "heading"));
    storage->layout()->addWidget(text("Profiles and settings are stored here. Keep this folder when updating the application.", "muted"));
    data_path_ = new QLabel(store_.path());
    data_path_->setTextFormat(Qt::PlainText);
    data_path_->setWordWrap(true);
    data_path_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    storage->layout()->addWidget(data_path_);
    storage->layout()->addWidget(text("Live screen capture is not included. Offline recordings support experimental recognition and minimap localization.", "muted"));
    settings_layout->addStretch();

    map_page_ = new MapPage(store_);
    auto* map_scroll = new QScrollArea;
    map_scroll->setWidgetResizable(true); map_scroll->setFrameShape(QFrame::NoFrame); map_scroll->setWidget(map_page_);
    pages_->addWidget(map_scroll);
    connect(map_page_, &MapPage::busyChanged, this, [this](bool busy) {
        selector_->setEnabled(!busy);
        for (auto* nav : nav_) { nav->setEnabled(!busy); }
    });
    connect(map_page_, &MapPage::databaseRestored, this, [this] { guarded([this] { load_profile(); }); });

    connect(selector_, &QComboBox::currentIndexChanged, this, [this] {
        guarded([&] { store_.activate_profile(selector_->currentData().toString()); load_profile(); });
    });
    for (auto* combo : {language_, theme_, scale_}) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { save_settings(); });
    }
    ready_ = true;
    load_profile();
    show_page(0);
    if (!store_.window_geometry().isEmpty()) {
        restoreGeometry(store_.window_geometry());
        bool visible = false;
        for (const auto* screen : QGuiApplication::screens()) {
            visible = visible || screen->availableGeometry().intersects(frameGeometry());
        }
        if (!visible && QGuiApplication::primaryScreen()) {
            move(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(32, 32));
        }
    }
}

MainWindow::~MainWindow() {
    ready_ = false;
    qApp->setFont(base_font_);
    qApp->setStyleSheet(QString());
}

void MainWindow::show_page(int index) {
    pages_->setCurrentIndex(index);
    for (int i = 0; i < static_cast<int>(nav_.size()); ++i) {
        nav_[static_cast<std::size_t>(i)]->setChecked(i == index);
    }
}

void MainWindow::show_example() {
    try {
        const QDir folder(QCoreApplication::applicationDirPath() + QStringLiteral("/examples/localization"));
        const auto read = [&](const QString& name, qint64 limit) {
            QFile file(folder.filePath(name));
            if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > limit) { throw std::runtime_error("The bundled example is missing or oversized."); }
            const auto bytes = file.read(limit + 1);
            if (bytes.size() > limit || file.error() != QFile::NoError) { throw std::runtime_error("Cannot read the bundled example."); }
            return bytes;
        };
        auto data = decode_json(read(QStringLiteral("map.json"), max_transfer_bytes));
        if (data.maps.size() != 1 || data.markers.size() != 2) { throw std::runtime_error("The bundled example has an unexpected layout."); }
        const auto png = read(QStringLiteral("reference.png"), 16 * 1024 * 1024); (void)decode_background(png);
        data.maps.first().title = tr("Training map");
        for (int i = 0; i < data.markers.size(); ++i) { data.markers[i].label = tr("Test chest %1").arg(i + 1); data.markers[i].status = CollectionStatus::NotCollected; }
        QTemporaryDir temporary(QFileInfo(store_.path()).absolutePath() + QStringLiteral("/example-XXXXXX"));
        if (!temporary.isValid()) { throw std::runtime_error("Cannot create temporary example storage."); }
        Store demo_store(temporary.filePath(QStringLiteral("example.db")));
        const auto profile = demo_store.active_profile().id; demo_store.save_settings(profile, settings_);
        demo_store.apply_import(data, profile, false, demo_store.review_import(data, profile, false).token);
        demo_store.set_background(data.maps.first().key, png, tr("Original example artwork"), QStringLiteral("MIT"));
        QDialog dialog(this); dialog.setObjectName(QStringLiteral("exampleWindow")); dialog.setWindowTitle(tr("Automatic tracking example")); dialog.resize(1120, 800);
        auto* layout = new QVBoxLayout(&dialog);
        auto* note = new QLabel(tr("Temporary example · Two test chests · Live game tracking is not enabled")); note->setWordWrap(true); note->setTextFormat(Qt::PlainText); layout->addWidget(note);
        auto* scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto* map = new MapPage(demo_store); map->set_example_mode(); scroll->setWidget(map); layout->addWidget(scroll, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close); buttons->button(QDialogButtonBox::Close)->setText(tr("Back to start"));
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
        QTimer::singleShot(0, map, [map, folder] { map->analyze_recording_file(folder.filePath(QStringLiteral("recording.json"))); });
        dialog.exec();
    } catch (const std::exception& error) {
        QMessageBox box(QMessageBox::Warning, tr("Could not open the example"), tr("Extract the complete application folder and try again."), QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText); box.setDetailedText(QString::fromUtf8(error.what())); box.exec();
    }
}

void MainWindow::refresh_profiles() {
    const QSignalBlocker selector_block(selector_);
    const QSignalBlocker list_block(profiles_);
    selector_->clear();
    profiles_->clear();
    for (const auto& item : store_.profiles()) {
        selector_->addItem(item.name, item.id);
        auto* row = new QListWidgetItem(item.name, profiles_);
        row->setData(Qt::UserRole, item.id);
        if (item.id == profile_.id) {
            profiles_->setCurrentItem(row);
        }
    }
    selector_->setCurrentIndex(selector_->findData(profile_.id));
    rename_->setText(profile_.name);
}

void MainWindow::load_profile() {
    profile_ = store_.active_profile();
    settings_ = store_.settings(profile_.id);
    {
        const QSignalBlocker a(language_), b(theme_), c(scale_);
        language_->setCurrentIndex(language_->findData(settings_.language));
        theme_->setCurrentIndex(theme_->findData(settings_.theme));
        scale_->setCurrentIndex(scale_->findData(settings_.ui_scale));
    }
    refresh_profiles();
    apply_settings();
    if (map_page_) { map_page_->reload(); }
}

void MainWindow::save_settings() {
    if (!ready_) { return; }
    guarded([&] {
        const Settings updated{language_->currentData().toString(), theme_->currentData().toString(), scale_->currentData().toInt()};
        set_interface_language(updated.language, [&] { store_.save_settings(profile_.id, updated); });
        settings_ = updated;
        apply_settings();
    });
}

void MainWindow::apply_settings() {
    apply_interface_language(settings_.language);
    auto font = base_font_;
    font.setPointSizeF(base_font_.pointSizeF() * settings_.ui_scale / 100.0);
    qApp->setFont(font);
    findChild<QFrame*>(QStringLiteral("sidebar"))->setFixedWidth(220 * settings_.ui_scale / 100);
    const bool dark = settings_.theme == QStringLiteral("dark");
    const QString background = dark ? QStringLiteral("#101820") : QStringLiteral("#f2f5f5");
    const QString panel = dark ? QStringLiteral("#18232e") : QStringLiteral("#ffffff");
    const QString foreground = dark ? QStringLiteral("#e7eef4") : QStringLiteral("#172731");
    const QString muted = dark ? QStringLiteral("#a5b7c5") : QStringLiteral("#526570");
    const QString border = dark ? QStringLiteral("#2c3c49") : QStringLiteral("#d4dfe2");
    const QString input = dark ? QStringLiteral("#111c26") : QStringLiteral("#f5f8f9");
    const QString accent = dark ? QStringLiteral("#83d7ba") : QStringLiteral("#187454");
    qApp->setStyleSheet(QStringLiteral(
        "QWidget { color:%1; } QMainWindow,QScrollArea,QScrollArea>QWidget>QWidget { background:%2; }"
        "QFrame#sidebar,QFrame#header { background:%3; } QFrame#header { border-bottom:1px solid %5; }"
        "QFrame#card { background:%3; border:1px solid %5; border-radius:12px; }"
        "QLabel { background:transparent; } QLabel[kind=muted] { color:%4; }"
        "QLabel[kind=eyebrow] { color:%6; font-weight:600; } QLabel[kind=brand] { color:%6; font-size:22px; font-weight:700; }"
        "QLabel[kind=title] { font-size:%7px; font-weight:600; } QLabel[kind=heading] { font-size:%8px; font-weight:600; }"
        "QLabel[kind=badge] { color:%6; border:1px solid %5; border-radius:8px; padding:8px; }"
        "QPushButton { background:%9; border:1px solid %5; border-radius:7px; padding:10px 16px; text-align:left; }"
        "QPushButton:hover { border-color:%6; } QPushButton:focus { border:2px solid %6; }"
        "QPushButton:disabled { color:%4; } QPushButton[primary=true] { background:%6; color:%2; font-weight:600; }"
        "QPushButton[disclosure=true]:checked { border-color:%6; color:%6; }"
        "QPushButton[nav=true] { border:0; background:transparent; padding:13px 12px; }"
        "QPushButton[nav=true]:checked { background:%9; color:%6; border-left:3px solid %6; }"
        "QComboBox,QLineEdit,QListWidget,QTableWidget,QPlainTextEdit,QDoubleSpinBox { background:%9; color:%1; border:1px solid %5; border-radius:6px; padding:8px; }"
        "QHeaderView::section { background:%3; color:%1; border:1px solid %5; padding:6px; }"
        "QTableCornerButton::section { background:%3; border:1px solid %5; }"
        "QTabWidget::pane { background:%9; border:1px solid %5; }"
        "QTabBar::tab { background:%3; color:%4; border:1px solid %5; padding:8px 12px; }"
        "QTabBar::tab:selected { background:%9; color:%6; border-top:2px solid %6; }"
        "QTableWidget::item:selected { background:%5; color:%1; } QDialog { background:%3; }"
        "QComboBox:focus,QLineEdit:focus,QListWidget:focus { border:2px solid %6; }"
        "QComboBox QAbstractItemView { background:%3; color:%1; selection-background-color:%5; }"
        "QListWidget::item { padding:8px; } QListWidget::item:selected { background:%5; color:%1; border-radius:4px; }"
        "QStatusBar { background:%3; color:%4; border-top:1px solid %5; } QMessageBox { background:%3; }"
    ).arg(foreground, background, panel, muted, border, accent,
          QString::number(28 * settings_.ui_scale / 100), QString::number(17 * settings_.ui_scale / 100), input));
    retranslate();
}

void MainWindow::retranslate() {
    setWindowTitle(tr("Teyvat Companion · Development Preview"));
    for (const auto& [label, source] : labels_) {
        label->setText(tr(source));
    }
    for (const auto& [control, source] : buttons_) {
        control->setText(tr(source));
    }
    theme_->setItemText(0, tr("Dark"));
    theme_->setItemText(1, tr("Light"));
    new_name_->setPlaceholderText(tr("A name for this computer only"));
    new_name_->setAccessibleName(tr("New profile name"));
    rename_->setAccessibleName(tr("Rename active profile"));
    profiles_->setAccessibleName(tr("Choose a profile"));
    statusBar()->showMessage(tr("Saved on this computer · Independent community project"));
    if (map_page_) { QEvent language_change(QEvent::LanguageChange); QCoreApplication::sendEvent(map_page_, &language_change); }
}

void MainWindow::changeEvent(QEvent* event) {
    if (ready_ && event->type() == QEvent::LanguageChange) {
        const QSignalBlocker blocker(language_);
        settings_.language = current_interface_language();
        language_->setCurrentIndex(language_->findData(settings_.language));
        retranslate();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::guarded(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Could not save the change"),
            tr("The change was not completed. Existing data was preserved.\n\n%1").arg(QString::fromUtf8(error.what())));
        try {
            load_profile();
        } catch (const std::exception& reload_error) {
            QMessageBox::critical(this, tr("Could not save the change"), QString::fromUtf8(reload_error.what()));
            qApp->quit();
        }
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    try {
        store_.save_window_geometry(saveGeometry());
        event->accept();
    } catch (const std::exception& error) {
        QMessageBox::warning(this, tr("Could not save window position"), QString::fromUtf8(error.what()));
        event->accept();
    }
}

} // namespace teyvat
