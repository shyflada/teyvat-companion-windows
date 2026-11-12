#include "teyvat/hoyolab_sync.hpp"
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <functional>
#include <limits>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <WebView2.h>
#endif

static void initialize_site_resource() { Q_INIT_RESOURCE(hoyolab); }
namespace teyvat {
namespace {
bool valid_map(int id) { return id == 2 || id == 7 || id == 9 || id == 36; }
#ifdef Q_OS_WIN
QString map_url(int id) {
    return QStringLiteral("https://act.hoyolab.com/ys/app/interactive-map/index.html?lang=en-us#/map/%1?shown_types=17,44,45,46,269").arg(id);
}
bool official_url(const QString& address) {
    const QUrl url(address);
    return url.scheme() == QStringLiteral("https") && url.host() == QStringLiteral("act.hoyolab.com")
        && url.path() == QStringLiteral("/ys/app/interactive-map/index.html");
}
template<class Interface, class... Args> class Callback final : public Interface {
public:
    Callback(GUID iid, std::function<void(Args...)> fn) : iid_(iid), fn_(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) { return E_POINTER; } *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, iid_)) { *out = static_cast<Interface*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references_; if (!n) { delete this; } return n; }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { try { fn_(args...); return S_OK; } catch (...) { return E_FAIL; } }
private:
    ULONG references_ = 1;
    GUID iid_;
    std::function<void(Args...)> fn_;
};
constexpr GUID environment_iid{0x4e8a3389,0xc9d8,0x4bd2,{0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d}};
constexpr GUID controller_iid{0x6c4819f3,0xc9b7,0x4260,{0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c}};
constexpr GUID script_iid{0x49511172,0xcc67,0x4bca,{0x99,0x23,0x13,0x71,0x12,0xf4,0xc4,0xcc}};
constexpr GUID source_iid{0x3c067f9f,0x5388,0x4772,{0x8b,0x48,0x79,0xf7,0xef,0x1a,0xb3,0x7c}};
#endif
}
struct HoYoLabSync::State {
    Store& store;
    QSettings options;
    QWidget* host;
    QLabel *label, *address;
    QTimer timer;
    QString bound, script;
    const char* message = nullptr;
    int map = 2, loaded_map = 0;
    bool initializing = false, busy = false, stopped = false;
    QMap<QString, QSet<QString>> remote;
#ifdef Q_OS_WIN
    HMODULE loader = nullptr;
    ICoreWebView2Environment* environment = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* browser = nullptr;
    bool com = false;
#endif
    State(Store& s, QWidget* h, QLabel* l, QLabel* a) : store(s), options(QFileInfo(s.path()).dir().filePath(QStringLiteral("hoyolab-sync.ini")), QSettings::IniFormat), host(h), label(l), address(a) {
        options.setFallbacksEnabled(false); bound = options.value(QStringLiteral("boundProfile")).toString();
        load_remote();
    }
    QString progress_key(int map_id) const { return QStringLiteral("profiles/") + bound + QStringLiteral("/maps/%1").arg(map_id); }
    void load_remote() {
        remote.clear();
        for (int id : {2, 7, 9, 36}) {
            const auto values = options.value(progress_key(id)).toStringList(); if (values.size() > max_markers) { continue; }
            QSet<QString> ids;
            for (const auto& value : values) { bool ok = false; const auto n = value.toLongLong(&ok); if (ok && n > 0 && n <= 9007199254740991LL) { ids.insert(QString::number(n)); } }
            remote[QStringLiteral("hoyolab-map-%1").arg(id)] = ids;
        }
    }
    QString cursor_key() const { return QStringLiteral("profiles/") + bound + QStringLiteral("/cursor"); }
    bool advance(qint64 id, const QString& command = {}) {
        options.setValue(cursor_key(), id); options.setValue(cursor_key() + QStringLiteral("Command"), command);
        options.sync(); stopped = options.status() != QSettings::NoError; return !stopped;
    }
    ~State() {
#ifdef Q_OS_WIN
        if (controller) { controller->Close(); }
        if (browser) { browser->Release(); } if (controller) { controller->Release(); }
        if (environment) { environment->Release(); }
        if (loader) { FreeLibrary(loader); } if (com) { CoUninitialize(); }
#endif
    }
};

HoYoLabSync::HoYoLabSync(Store& store, QWidget* parent) : QWidget(parent, Qt::Window) {
    setObjectName(QStringLiteral("hoyolabConnection")); resize(1100, 780);
    auto* layout = new QVBoxLayout(this);
    auto* address = new QLabel(QStringLiteral("https://act.hoyolab.com/ys/app/interactive-map/index.html")); address->setTextFormat(Qt::PlainText); address->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(address);
    auto* label = new QLabel; label->setWordWrap(true); label->setTextFormat(Qt::PlainText); layout->addWidget(label);
    auto* host = new QWidget; host->setAttribute(Qt::WA_NativeWindow); host->setMinimumSize(400, 300); layout->addWidget(host, 1);
    state_ = std::make_unique<State>(store, host, label, address); host->installEventFilter(this);
    initialize_site_resource(); QFile script(QStringLiteral(":/hoyolab/hoyolab_site.js"));
    if (!script.open(QIODevice::ReadOnly)) { throw std::runtime_error("Official map UI adapter is missing."); }
    state_->script = QString::fromUtf8(script.readAll());
    continue_ = new QPushButton; layout->addWidget(continue_); connect(continue_, &QPushButton::clicked, this, &QWidget::hide);
    retranslate();
    connect(&state_->timer, &QTimer::timeout, this, &HoYoLabSync::poll); state_->timer.setInterval(1000);
    status(QT_TR_NOOP("Sign in on the official website. Local confirmations will then sync automatically."));
    if (state_->options.status() == QSettings::NoError && state_->options.value(QStringLiteral("enabled"), false).toBool()) {
        QTimer::singleShot(0, this, &HoYoLabSync::initialize);
    }
}
HoYoLabSync::~HoYoLabSync() = default;
void HoYoLabSync::status(const char* source) {
    auto& s = *state_;
    s.message = source;
    const auto text = tr(source);
    if (s.label->text() == text) { return; }
    s.label->setText(text);
    emit status_changed(text);
}
void HoYoLabSync::retranslate() {
    setWindowTitle(tr("Official HoYoLAB map"));
    continue_->setText(tr("Continue in game"));
    if (state_->message) { status(state_->message); }
}
void HoYoLabSync::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && continue_) { retranslate(); }
    QWidget::changeEvent(event);
}
void HoYoLabSync::open(int id) {
    use_map(id); auto& s = *state_;
    if (s.options.status() != QSettings::NoError) { status(QT_TR_NOOP("Cannot read sync settings. Local progress is safe; sync is paused.")); show(); return; }
    const auto profile = s.store.active_profile().id;
    if (s.bound != profile) { s.bound = profile; s.load_remote(); for (const auto& source : s.remote.keys()) { emit progress_changed(source, s.remote.value(source)); } }
    s.options.setValue(QStringLiteral("boundProfile"), s.bound);
    s.options.setValue(QStringLiteral("enabled"), true); s.options.sync();
    if (s.options.status() != QSettings::NoError) { s.stopped = true; status(QT_TR_NOOP("Cannot save sync settings. Local progress is safe; sync is paused.")); }
    show(); raise(); activateWindow(); initialize();
}
void HoYoLabSync::use_map(int id) { if (valid_map(id)) { state_->map = id; } }
QSet<QString> HoYoLabSync::collected(const QString& source) const {
    if (state_->store.active_profile().id != state_->bound) { return {}; } return state_->remote.value(source);
}
void HoYoLabSync::initialize() {
    auto& s = *state_;
#ifdef Q_OS_WIN
    if (s.browser || s.initializing || s.stopped) { return; } s.initializing = true;
    if (s.controller) { s.controller->Close(); s.controller->Release(); s.controller = nullptr; }
    if (s.environment) { s.environment->Release(); s.environment = nullptr; }
    const auto init = s.com ? S_FALSE : CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init)) { s.initializing = false; status(QT_TR_NOOP("Could not start the official map browser. Local confirmations remain queued.")); return; } s.com = true;
    const auto library = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + QStringLiteral("/WebView2Loader.dll")).toStdWString();
    if (!s.loader) { s.loader = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32); }
    using Create = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    const auto create = s.loader ? reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(s.loader, "CreateCoreWebView2EnvironmentWithOptions"))) : nullptr;
    if (!create) { s.initializing = false; status(QT_TR_NOOP("WebView2Loader.dll is missing. Extract the complete beta package.")); return; }
    const auto directory = QDir::toNativeSeparators(QFileInfo(s.store.path()).dir().filePath(QStringLiteral("hoyolab-browser"))).toStdWString();
    QPointer<HoYoLabSync> guard(this);
    auto* callback = new Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(environment_iid, [guard](HRESULT error, ICoreWebView2Environment* environment) {
        if (!guard) { return; } auto& s = *guard->state_;
        if (FAILED(error) || !environment) { s.initializing = false; guard->status(QT_TR_NOOP("Microsoft Edge WebView2 Runtime is required. Local confirmations remain queued.")); return; }
        s.environment = environment; environment->AddRef();
        auto* controller = new Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(controller_iid, [guard](HRESULT error, ICoreWebView2Controller* controller) {
            if (!guard) { return; } auto& s = *guard->state_; s.initializing = false;
            if (FAILED(error) || !controller) { guard->status(QT_TR_NOOP("Could not open the official map. Local confirmations remain queued.")); return; }
            s.controller = controller; controller->AddRef();
            if (FAILED(controller->get_CoreWebView2(&s.browser)) || !s.browser) { guard->status(QT_TR_NOOP("Could not open the official map. Local confirmations remain queued.")); return; }
            auto* changed = new Callback<ICoreWebView2SourceChangedEventHandler, ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*>(source_iid, [guard](ICoreWebView2* browser, ICoreWebView2SourceChangedEventArgs*) {
                if (!guard) { return; } LPWSTR value = nullptr;
                if (SUCCEEDED(browser->get_Source(&value)) && value) {
                    const QUrl url(QString::fromWCharArray(value)); CoTaskMemFree(value);
                    guard->state_->address->setText(url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo).toDisplayString());
                }
            });
            EventRegistrationToken token{}; s.browser->add_SourceChanged(changed, &token); changed->Release();
            RECT bounds{}; GetClientRect(reinterpret_cast<HWND>(s.host->winId()), &bounds); controller->put_Bounds(bounds); controller->put_IsVisible(TRUE);
            const auto url = map_url(s.map).toStdWString(); s.loaded_map = s.map; s.browser->Navigate(url.c_str()); s.timer.start();
        });
        const auto result = environment->CreateCoreWebView2Controller(reinterpret_cast<HWND>(s.host->winId()), controller); controller->Release();
        if (FAILED(result)) { s.initializing = false; guard->status(QT_TR_NOOP("Could not open the official map. Local confirmations remain queued.")); }
    });
    const auto error = create(nullptr, directory.c_str(), nullptr, callback); callback->Release();
    if (FAILED(error)) { s.initializing = false; status(QT_TR_NOOP("Microsoft Edge WebView2 Runtime is required. Local confirmations remain queued.")); }
#else
    status(QT_TR_NOOP("Official account sync requires the Windows beta."));
#endif
}
void HoYoLabSync::poll() {
    auto& s = *state_;
#ifdef Q_OS_WIN
    if (!s.browser || s.busy || s.stopped) { return; }
    if (s.store.active_profile().id != s.bound) { status(QT_TR_NOOP("Local profile changed. Connect HoYoLAB again for this profile.")); return; }
    try {
        auto cursor = s.options.value(s.cursor_key(), 0).toLongLong();
        if (cursor < 0 || cursor >= std::numeric_limits<qint64>::max() || s.options.status() != QSettings::NoError) { s.stopped = true; status(QT_TR_NOOP("Cannot read sync settings. Local progress is safe; sync is paused.")); return; }
        // A restored database may reuse history IDs. Replaying is idempotent;
        // skipping an unacknowledged new command would lose a confirmation.
        if (cursor > 0) {
            const auto previous = s.store.history(s.bound, 1, cursor + 1);
            if (previous.isEmpty() || previous.first().id != cursor || previous.first().command != s.options.value(s.cursor_key() + QStringLiteral("Command")).toString()) {
                if (!s.advance(0)) { return; } cursor = 0;
            }
        }
        const auto history = s.store.history_after(s.bound, cursor);
        std::optional<HistoryEvent> pending;
        for (qsizetype i = 0; i < history.size(); ++i) {
            const auto& h = history[i];
            const auto match = QRegularExpression(QStringLiteral("^hoyolab-map-(2|7|9|36)$")).match(h.source);
            bool valid = false; const auto id = h.marker_id.toLongLong(&valid);
            bool superseded = false;
            for (qsizetype j = i + 1; j < history.size(); ++j) { if (history[j].source == h.source && history[j].marker_id == h.marker_id) { superseded = true; break; } }
            if (!match.hasMatch() || !valid || id <= 0 || id > 9007199254740991LL || superseded || h.origin == QStringLiteral("import")
                || is_confirmed(read_status(h.before)) == is_confirmed(read_status(h.after))) {
                if (!s.advance(h.id, h.command)) { status(QT_TR_NOOP("Cannot save sync settings. Local progress is safe; sync is paused.")); return; } continue;
            }
            pending = h; break;
        }
        int map = s.map; QJsonObject request{{QStringLiteral("write"), false}};
        if (pending) {
            map = pending->source.mid(12).toInt(); request[QStringLiteral("write")] = true;
            request[QStringLiteral("point")] = pending->marker_id.toLongLong(); request[QStringLiteral("collected")] = is_confirmed(read_status(pending->after));
        }
        if (!valid_map(map)) { status(QT_TR_NOOP("A queued marker has an invalid map. Sync is paused.")); s.stopped = true; return; }
        if (s.loaded_map != map) { const auto url = map_url(map).toStdWString(); s.browser->Navigate(url.c_str()); s.loaded_map = map; return; }
        LPWSTR address = nullptr; if (FAILED(s.browser->get_Source(&address))) { return; }
        const auto source = QString::fromWCharArray(address); CoTaskMemFree(address);
        if (!official_url(source)) { status(QT_TR_NOOP("Sign in on the official website to resume queued confirmations.")); return; }
        request[QStringLiteral("map")] = map;
        const auto script = (QLatin1Char('(') + s.script + QStringLiteral(")(") + QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)) + QLatin1Char(')')).toStdWString();
        s.busy = true; const auto bound = s.bound; QPointer<HoYoLabSync> guard(this);
        auto* callback = new Callback<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(script_iid, [guard, bound, pending, map](HRESULT error, LPCWSTR json) {
            if (!guard) { return; } auto& s = *guard->state_; s.busy = false;
            if (s.bound != bound || s.store.active_profile().id != bound) { return; }
            if (FAILED(error) || !json) { guard->status(QT_TR_NOOP("Official map is unavailable. Local confirmations remain queued.")); return; }
            const auto object = QJsonDocument::fromJson(QString::fromWCharArray(json).toUtf8()).object(); const auto state = object.value(QStringLiteral("state")).toString();
            if (object.value(QStringLiteral("ids")).isArray()) {
                const auto values = object.value(QStringLiteral("ids")).toArray(); QSet<QString> ids;
                if (values.size() > max_markers) { guard->status(QT_TR_NOOP("Official map response changed. Local confirmations remain queued.")); return; }
                for (const auto& value : values) { const auto id = value.toInteger(); if (id <= 0 || id > 9007199254740991LL) { return; } ids.insert(QString::number(id)); }
                const auto source = QStringLiteral("hoyolab-map-%1").arg(map);
                if (s.remote.value(source) != ids) {
                    s.remote[source] = ids; s.options.setValue(s.progress_key(map), QStringList(ids.begin(), ids.end())); s.options.sync();
                    if (s.options.status() != QSettings::NoError) { s.stopped = true; guard->status(QT_TR_NOOP("Cannot save sync settings. Local progress is safe; sync is paused.")); return; }
                    emit guard->progress_changed(source, ids);
                }
            }
            if (state == QStringLiteral("ready") || state == QStringLiteral("ack")) {
                if (state == QStringLiteral("ack")) {
                    if (!pending || object.value(QStringLiteral("point")).toInteger() != pending->marker_id.toLongLong()
                        || !object.value(QStringLiteral("collected")).isBool() || object.value(QStringLiteral("collected")).toBool() != is_confirmed(read_status(pending->after))) { return; }
                    if (!s.advance(pending->id, pending->command)) { guard->status(QT_TR_NOOP("Cannot save sync settings. Local progress is safe; sync is paused.")); return; }
                    guard->status(QT_TR_NOOP("Chest status saved on the official map."));
                } else { guard->status(QT_TR_NOOP("HoYoLAB connected. Collected pins are excluded from navigation.")); }
            } else if (state == QStringLiteral("login_required")) {
                if (!s.remote.isEmpty()) { const auto sources = s.remote.keys(); s.remote.clear(); for (int id : {2, 7, 9, 36}) { s.options.remove(s.progress_key(id)); } s.options.sync(); for (const auto& source : sources) { emit guard->progress_changed(source, {}); } }
                guard->status(QT_TR_NOOP("Sign in on the official website. Local confirmations remain queued."));
            } else if (state == QStringLiteral("pending") || state == QStringLiteral("selecting") || state == QStringLiteral("busy")) {
                guard->status(QT_TR_NOOP("Saving chest status on the official map…"));
            } else if (state == QStringLiteral("loading")) { guard->status(QT_TR_NOOP("Loading the official map…")); }
            else if (state == QStringLiteral("wrong_map")) { s.loaded_map = 0; }
            else { guard->status(QT_TR_NOOP("Official map has not confirmed this pin. Open the map window; local progress is saved and queued.")); }
        });
        const auto result = s.browser->ExecuteScript(script.c_str(), callback); callback->Release();
        if (FAILED(result)) { s.busy = false; status(QT_TR_NOOP("Official map is unavailable. Local confirmations remain queued.")); }
    } catch (const std::exception&) { s.stopped = true; status(QT_TR_NOOP("Cannot read local progress. Sync is paused; local data is preserved.")); }
#endif
}
bool HoYoLabSync::eventFilter(QObject* object, QEvent* event) {
#ifdef Q_OS_WIN
    if (object == state_->host && event->type() == QEvent::Resize && state_->controller) {
        RECT bounds{}; GetClientRect(reinterpret_cast<HWND>(state_->host->winId()), &bounds); state_->controller->put_Bounds(bounds);
    }
    if (event->type() == QEvent::Move && state_->controller) { state_->controller->NotifyParentWindowPositionChanged(); }
#endif
    return QWidget::eventFilter(object, event);
}
void HoYoLabSync::closeEvent(QCloseEvent* event) { hide(); event->ignore(); }
} // namespace teyvat
