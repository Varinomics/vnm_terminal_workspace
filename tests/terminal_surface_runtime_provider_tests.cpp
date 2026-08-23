#include "terminal_worker_runtime_internal.h"

#include "vnm_terminal/vnm_terminal_surface.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QTextStream>

#include <Windows.h>
#include <TlHelp32.h>

#include <condition_variable>
#include <cstdio>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace workspace = vnm::terminal_workspace;
namespace runtime_detail = vnm::terminal_workspace::detail;
namespace environment = vnm::environment_policy;

namespace {

constexpr std::string_view k_parent_canary_name =
    "VNM_WORKSPACE_PROVIDER_PARENT_ONLY_CANARY";

bool check(bool condition, std::string_view message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "FAIL: %.*s\n",
        static_cast<int>(message.size()), message.data());
    return false;
}

bool process_gui_events_until(const std::function<bool()>& condition)
{
    QDeadlineTimer deadline(5000);
    do {
        QCoreApplication::processEvents();
        if (condition()) {
            return true;
        }
    } while (!deadline.hasExpired());
    return condition();
}

std::string utf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString environment_value(const wchar_t* name)
{
    SetLastError(ERROR_SUCCESS);
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0U);
    if (required == 0U) {
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND
            ? QStringLiteral("<absent>")
            : QString();
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name,
        value.data(),
        required);
    if (written == 0U || written >= required) {
        return QString();
    }
    value.resize(written);
    return QString::fromStdWString(value);
}

int run_child(const QString& evidence_path)
{
    QFile evidence(evidence_path);
    if (!evidence.open(QIODevice::WriteOnly | QIODevice::Truncate |
        QIODevice::Text))
    {
        return 91;
    }
    wchar_t working_directory[MAX_PATH + 1]{};
    const DWORD working_directory_length = GetCurrentDirectoryW(
        MAX_PATH + 1,
        working_directory);
    if (working_directory_length == 0U ||
        working_directory_length > MAX_PATH)
    {
        return 92;
    }

    QTextStream stream(&evidence);
    stream << "pid=" << GetCurrentProcessId() << '\n';
    stream << "cwd=" << QString::fromWCharArray(working_directory) << '\n';
    stream << "base=" << environment_value(L"WORKSPACE_PROVIDER_BASE") << '\n';
    stream << "authorized="
           << environment_value(L"WORKSPACE_PROVIDER_AUTHORIZED") << '\n';
    stream << "parent="
           << environment_value(
                  L"VNM_WORKSPACE_PROVIDER_PARENT_ONLY_CANARY")
           << '\n';
    stream << "term=" << environment_value(L"TERM") << '\n';
    stream << "colorterm=" << environment_value(L"COLORTERM") << '\n';
    stream.flush();
    evidence.close();
    return 17;
}

class Parent_environment_guard
{
public:
    Parent_environment_guard()
    :
        m_was_set(qEnvironmentVariableIsSet(k_parent_canary_name.data())),
        m_value(qgetenv(k_parent_canary_name.data()))
    {
        qputenv(k_parent_canary_name.data(), "must-not-be-inherited");
    }

    ~Parent_environment_guard()
    {
        if (m_was_set) {
            qputenv(k_parent_canary_name.data(), m_value);
        } else {
            qunsetenv(k_parent_canary_name.data());
        }
    }

private:
    bool m_was_set = false;
    QByteArray m_value;
};

QString normalized_image_path(const QString& path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return QDir::toNativeSeparators(canonical.isEmpty() ? path : canonical);
}

std::size_t process_count_for_image(const QString& expected_image)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return static_cast<std::size_t>(-1);
    }
    const QString normalized_expected = normalized_image_path(expected_image);
    std::size_t count = 0U;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            wchar_t image_path[32768]{};
            DWORD image_path_size = static_cast<DWORD>(std::size(image_path));
            const bool queried = QueryFullProcessImageNameW(
                process,
                0U,
                image_path,
                &image_path_size) != FALSE;
            CloseHandle(process);
            if (queried && normalized_image_path(
                    QString::fromWCharArray(
                        image_path,
                        static_cast<qsizetype>(image_path_size))).compare(
                    normalized_expected,
                    Qt::CaseInsensitive) == 0)
            {
                ++count;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

class Admission_barrier
{
public:
    runtime_detail::Terminal_native_start_admission wait()
    {
        std::unique_lock lock(m_mutex);
        m_entered = true;
        m_condition.notify_all();
        m_condition.wait(lock, [this] { return m_released; });
        return m_result;
    }

    void wait_until_entered()
    {
        std::unique_lock lock(m_mutex);
        m_condition.wait(lock, [this] { return m_entered; });
    }

    void release(runtime_detail::Terminal_native_start_admission result)
    {
        std::lock_guard lock(m_mutex);
        m_result = result;
        m_released = true;
        m_condition.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    runtime_detail::Terminal_native_start_admission m_result =
        runtime_detail::Terminal_native_start_admission::CANCEL;
    bool m_entered = false;
    bool m_released = false;
};

class Immediate_gui_dispatcher final :
    public workspace::Terminal_worker_gui_dispatcher
{
public:
    bool dispatch(
        workspace::Terminal_gui_dispatch_kind kind,
        const std::function<void()>& function) override
    {
        if (fail_dispatch) {
            return false;
        }
        if (kind == workspace::Terminal_gui_dispatch_kind::BLOCKING) {
            ++blocking_calls;
            function();
            return true;
        }
        ++queued_calls;
        return QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            function,
            Qt::QueuedConnection);
    }

    bool fail_dispatch = false;
    int blocking_calls = 0;
    int queued_calls = 0;
};

class Recording_remote_sink final : public workspace::Terminal_worker_remote_sink
{
public:
    std::optional<workspace::Terminal_remote_surface_identity> create_surface(
        const workspace::Terminal_remote_surface_descriptor& descriptor) override
    {
        ++create_calls;
        last_descriptor = descriptor;
        if (fail_create) {
            return std::nullopt;
        }
        return 41U;
    }

    std::optional<workspace::Terminal_remote_frame_buffer> begin_frame(
        workspace::Terminal_remote_surface_identity surface,
        std::int32_t width,
        std::int32_t height) override
    {
        ++begin_calls;
        last_surface = surface;
        last_frame_width = width;
        last_frame_height = height;
        const std::int32_t stride = width * 4;
        frame.resize(
            static_cast<std::size_t>(stride) *
            static_cast<std::size_t>(height));
        return workspace::Terminal_remote_frame_buffer{
            frame,
            stride,
        };
    }

    void end_frame(
        workspace::Terminal_remote_surface_identity surface,
        std::span<const workspace::Terminal_remote_damage_rectangle> damage,
        std::uint32_t flags) override
    {
        ++end_calls;
        last_surface = surface;
        last_damage_count = damage.size();
        last_present_flags = flags;
    }

    void destroy_surface(
        workspace::Terminal_remote_surface_identity surface) override
    {
        ++destroy_calls;
        last_surface = surface;
    }

    void set_cursor(
        workspace::Terminal_remote_surface_identity surface,
        std::int32_t cursor_shape) override
    {
        ++cursor_calls;
        last_surface = surface;
        last_cursor = cursor_shape;
    }

    workspace::Terminal_remote_initial_state initial_state() override
    {
        return configured_initial_state;
    }

    bool fail_create = false;
    workspace::Terminal_remote_initial_state configured_initial_state;
    workspace::Terminal_remote_surface_descriptor last_descriptor;
    std::vector<std::uint8_t> frame;
    workspace::Terminal_remote_surface_identity last_surface = 0U;
    std::size_t last_damage_count = 0U;
    std::uint32_t last_present_flags = 0U;
    std::int32_t last_frame_width = 0;
    std::int32_t last_frame_height = 0;
    std::int32_t last_cursor = 0;
    int create_calls = 0;
    int begin_calls = 0;
    int end_calls = 0;
    int destroy_calls = 0;
    int cursor_calls = 0;
};

class Recording_termination final : public workspace::Terminal_worker_termination
{
public:
    void terminate_hosted_worker() override
    {
        ++calls;
    }

    int calls = 0;
};

class Recording_transport final : public workspace::Terminal_child_fact_transport
{
public:
    workspace::Terminal_child_fact_delivery_result deliver(
        const workspace::Terminal_child_fact& fact) override
    {
        delivered.push_back(fact);
        return {
            workspace::Terminal_child_fact_delivery_status::DELIVERED,
            workspace::Terminal_child_fact_acknowledgement::ACCEPTED,
        };
    }

    std::vector<workspace::Terminal_child_fact> delivered;
};

struct Fixture_paths
{
    QString child_directory;
    QString child_image;
    QString working_directory;
    QString evidence;
};

std::optional<Fixture_paths> create_fixture(QTemporaryDir& directory)
{
    Fixture_paths paths{
        directory.filePath(QStringLiteral("bin")),
        {},
        directory.filePath(QStringLiteral("working")),
        directory.filePath(QStringLiteral("child-evidence.txt")),
    };
    if (!QDir().mkpath(paths.child_directory) ||
        !QDir().mkpath(paths.working_directory))
    {
        return std::nullopt;
    }
    paths.child_image = QDir(paths.child_directory).filePath(
        QStringLiteral("workspace-provider-child.exe"));
    if (!QFile::copy(QCoreApplication::applicationFilePath(), paths.child_image)) {
        return std::nullopt;
    }
    return paths;
}

std::vector<environment::Environment_entry> launch_environment(
    const Fixture_paths& paths,
    std::optional<std::string> path_override = std::nullopt)
{
    const QString system_root = qEnvironmentVariable("SystemRoot");
    const QString path = path_override
        ? QString::fromUtf8(path_override->data(),
              static_cast<qsizetype>(path_override->size()))
        : paths.child_directory + QLatin1Char(';') +
            QLibraryInfo::path(QLibraryInfo::BinariesPath) + QLatin1Char(';') +
            QDir(system_root).filePath(QStringLiteral("System32"));
    return {
        {"PATH", utf8(path)},
        {"PATHEXT", ".COM;.EXE;.BAT;.CMD"},
        {"SystemRoot", utf8(system_root)},
        {"WINDIR", utf8(system_root)},
        {"WORKSPACE_PROVIDER_BASE", "base-value"},
    };
}

workspace::Launch_request_result request_payload(
    std::string executable,
    const Fixture_paths& paths,
    std::vector<environment::Environment_entry> base_environment)
{
    workspace::Terminal_launch_request request;
    request.launch_request_id = "provider-request";
    request.session_id = "provider-session";
    request.argv = {
        std::move(executable),
        "--workspace-provider-child",
        utf8(paths.evidence),
    };
    request.working_directory = utf8(paths.working_directory);
    request.base_environment_complete = true;
    request.base_environment = std::move(base_environment);
    request.cancellation.identity = "provider-cancellation";
    return workspace::prepare_terminal_launch_request(
        std::move(request),
        workspace::Launch_platform::WINDOWS);
}

workspace::Terminal_worker_surface_configuration provider_configuration()
{
    workspace::Terminal_worker_surface_configuration configuration;
    configuration.logical_width = 800;
    configuration.logical_height = 600;
    configuration.settings.font_family = "Consolas";
    configuration.settings.font_size = 14.0;
    configuration.settings.scrollback_limit = 128;
    configuration.title = "Neutral terminal worker";
    configuration.scrollbar_width = 12.0;
    return configuration;
}

bool provider_construction_was_neutral_and_complete(
    const runtime_detail::Terminal_surface_runtime_adapter& provider,
    const Recording_remote_sink& sink,
    const Immediate_gui_dispatcher& dispatcher,
    const Recording_termination& termination)
{
    const auto observation = provider.test_observation();
    return observation.surface_construction_count == 1U &&
        observation.surface_alive && observation.scrollbar_alive &&
        observation.remote_runtime_initialized &&
        sink.create_calls == 1 && dispatcher.blocking_calls >= 1 &&
        termination.calls == 1;
}

bool remote_runtime_value_controls_and_teardown_are_owned()
{
    Recording_remote_sink sink;
    sink.configured_initial_state.logical_width = 420;
    sink.configured_initial_state.logical_height = 240;
    sink.configured_initial_state.scale_factor = 1.0F;
    Immediate_gui_dispatcher dispatcher;
    Recording_termination termination;
    runtime_detail::Terminal_surface_runtime_adapter provider(
        provider_configuration(),
        sink,
        dispatcher,
        termination);

    bool ok = true;
    ok &= check(
        provider.initialize() ==
            workspace::Terminal_worker_initialization_result::READY,
        "real remote runtime must initialize the fixed packaged worker root");
    const auto initialized = provider.test_observation();
    ok &= check(
        initialized.surface_construction_count == 1U &&
            initialized.surface_alive && initialized.scrollbar_alive &&
            initialized.remote_runtime_initialized &&
            initialized.surface_has_focus &&
            initialized.surface_width == 408.0 &&
            initialized.surface_height == 240.0 &&
            initialized.scrollbar_width == 12.0 &&
            sink.create_calls == 1,
        "after_load must privately construct and lay out one focused terminal surface");

    workspace::Terminal_remote_input_message input;
    input.event_type = 1U;
    input.x = 20;
    input.y = 30;
    workspace::Terminal_remote_state_message resize;
    resize.state_type = 1U;
    resize.width = 500;
    resize.height = 300;
    const bool controls_admitted = provider.forward_input(input) &&
        provider.forward_state(resize) && provider.request_present();
    const bool controls_processed = process_gui_events_until([&] {
        const auto observation = provider.test_observation();
        return observation.surface_width == 488.0 &&
            observation.surface_height == 300.0 &&
            sink.begin_calls >= 1 && sink.end_calls >= 1;
    });
    ok &= check(
        controls_admitted && controls_processed &&
            provider.test_inject_timestamp_request(),
        "value input/state/present and internal timestamp connections must forward");
    const auto updated = provider.test_observation();
    ok &= check(
        updated.surface_width == 488.0 && updated.surface_height == 300.0 &&
            updated.timestamp_visible && dispatcher.queued_calls >= 2 &&
            sink.begin_calls >= 1 && sink.end_calls >= 1,
        "resize/present must reach the real runtime, internal layout, and sink");

    provider.shutdown();
    const auto shutdown = provider.test_observation();
    ok &= check(
        !shutdown.surface_alive && !shutdown.scrollbar_alive &&
            !shutdown.remote_runtime_initialized && sink.destroy_calls == 1 &&
            shutdown.private_teardown_order > 0U &&
            shutdown.private_teardown_order < shutdown.remote_shutdown_order,
        "surface and scrollbar must tear down before remote runtime shutdown");

    Recording_remote_sink failed_sink;
    failed_sink.fail_create = true;
    Immediate_gui_dispatcher failed_sink_dispatcher;
    Recording_termination failed_sink_termination;
    runtime_detail::Terminal_surface_runtime_adapter failed_sink_provider(
        provider_configuration(),
        failed_sink,
        failed_sink_dispatcher,
        failed_sink_termination);
    ok &= check(
        failed_sink_provider.initialize() ==
            workspace::Terminal_worker_initialization_result::
                REMOTE_RUNTIME_FAILED &&
            failed_sink_provider.test_observation().surface_construction_count ==
                0U,
        "sink creation failure must not expose or construct a terminal surface");

    Recording_remote_sink failed_dispatch_sink;
    Immediate_gui_dispatcher failed_dispatcher;
    failed_dispatcher.fail_dispatch = true;
    Recording_termination failed_dispatch_termination;
    runtime_detail::Terminal_surface_runtime_adapter failed_dispatch_provider(
        provider_configuration(),
        failed_dispatch_sink,
        failed_dispatcher,
        failed_dispatch_termination);
    ok &= check(
        failed_dispatch_provider.initialize() ==
            workspace::Terminal_worker_initialization_result::
                REMOTE_RUNTIME_FAILED &&
            failed_dispatch_provider.test_observation().surface_construction_count ==
                0U &&
            failed_dispatch_sink.destroy_calls == 1,
        "GUI dispatch failure must settle the remote surface without a root");
    return ok;
}

bool pre_native_cancel_allocates_no_backend_or_pid()
{
    QTemporaryDir directory;
    const std::optional<Fixture_paths> fixture = create_fixture(directory);
    if (!check(fixture.has_value(), "cancellation fixture must be created")) {
        return false;
    }
    const workspace::Launch_request_result prepared = request_payload(
        "workspace-provider-child.exe",
        *fixture,
        launch_environment(*fixture));
    if (!check(prepared.status == workspace::Launch_request_status::ACCEPTED,
        "cancellation request must encode"))
    {
        return false;
    }

    Admission_barrier barrier;
    Recording_remote_sink sink;
    Immediate_gui_dispatcher dispatcher;
    Recording_transport transport;
    Recording_termination termination;
    runtime_detail::Terminal_surface_runtime_adapter provider(
        provider_configuration(),
        sink,
        dispatcher,
        termination,
        [&barrier] { return barrier.wait(); });
    runtime_detail::Terminal_worker_coordinator runtime(provider, transport);
    const auto initialized = provider.initialize();
    std::size_t process_count_while_held = static_cast<std::size_t>(-1);
    std::thread observer([&] {
        barrier.wait_until_entered();
        process_count_while_held = process_count_for_image(fixture->child_image);
        barrier.release(
            runtime_detail::Terminal_native_start_admission::CANCEL);
    });

    const workspace::Terminal_worker_run_result result =
        runtime.run_preinitialized(
            prepared.serialized_request,
            workspace::Launch_platform::WINDOWS,
            101U);
    observer.join();

    return check(
        initialized == workspace::Terminal_worker_initialization_result::READY &&
            result == workspace::Terminal_worker_run_result::TERMINATED &&
            process_count_while_held == 0U &&
            process_count_for_image(fixture->child_image) == 0U &&
            !QFileInfo::exists(fixture->evidence) &&
            provider.test_observation().process_state ==
                static_cast<int>(
                    VNM_TerminalSurface::Process_state::NOT_STARTED) &&
            provider_construction_was_neutral_and_complete(
                provider,
                sink,
                dispatcher,
                termination) &&
            transport.delivered.size() == 1U &&
            transport.delivered.front().kind ==
                workspace::Terminal_child_fact_kind::START_FAILED &&
            transport.delivered.front().error ==
                workspace::Terminal_child_fact_error::CANCELLED &&
            !transport.delivered.front().native_dispatch_occurred,
        "hold/cancel immediately before native start must allocate no backend or PID");
}

bool released_start_dispatches_once_with_exact_environment()
{
    Parent_environment_guard parent_environment;
    QTemporaryDir directory;
    const std::optional<Fixture_paths> fixture = create_fixture(directory);
    if (!check(fixture.has_value(), "release fixture must be created")) {
        return false;
    }
    const workspace::Launch_request_result prepared = request_payload(
        "workspace-provider-child.exe",
        *fixture,
        launch_environment(*fixture));
    if (!check(prepared.status == workspace::Launch_request_status::ACCEPTED,
        "release request must encode"))
    {
        return false;
    }

    Admission_barrier barrier;
    Recording_remote_sink sink;
    Immediate_gui_dispatcher dispatcher;
    Recording_transport transport;
    Recording_termination termination;
    runtime_detail::Terminal_surface_runtime_adapter provider(
        provider_configuration(),
        sink,
        dispatcher,
        termination,
        [&barrier] { return barrier.wait(); });
    runtime_detail::Terminal_worker_coordinator runtime(provider, transport);
    const auto initialized = provider.initialize();
    std::size_t process_count_while_held = static_cast<std::size_t>(-1);
    std::thread observer([&] {
        barrier.wait_until_entered();
        process_count_while_held = process_count_for_image(fixture->child_image);
        barrier.release(
            runtime_detail::Terminal_native_start_admission::RELEASE);
    });
    const std::optional<std::vector<environment::Environment_entry>> authorized =
        std::vector<environment::Environment_entry>{
            {"WORKSPACE_PROVIDER_AUTHORIZED", "authorized-value"},
        };

    const workspace::Terminal_worker_run_result result =
        runtime.run_preinitialized(
            prepared.serialized_request,
            workspace::Launch_platform::WINDOWS,
            102U,
            {},
            authorized);
    observer.join();
    QFile evidence(fixture->evidence);
    const bool evidence_opened = evidence.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString evidence_text = evidence_opened
        ? QString::fromUtf8(evidence.readAll())
        : QString();

    bool ok = true;
    ok &= check(
        initialized == workspace::Terminal_worker_initialization_result::READY &&
            result == workspace::Terminal_worker_run_result::COMPLETED &&
            process_count_while_held == 0U &&
            provider_construction_was_neutral_and_complete(
                provider,
                sink,
                dispatcher,
                termination),
        "release must cross one provider/native dispatch after a zero-PID hold");
    ok &= check(
        transport.delivered.size() == 2U &&
            transport.delivered[0].kind ==
                workspace::Terminal_child_fact_kind::STARTED &&
            transport.delivered[0].native_dispatch_occurred &&
            transport.delivered[1].kind ==
                workspace::Terminal_child_fact_kind::EXITED &&
            transport.delivered[1].exit_code == 17,
        "real provider result must publish one dispatched start and controlled exit");
    ok &= check(
        evidence_opened &&
            evidence_text.count(QStringLiteral("pid=")) == 1 &&
            evidence_text.contains(QStringLiteral("cwd=") +
                QDir::toNativeSeparators(fixture->working_directory)) &&
            evidence_text.contains(QStringLiteral("base=base-value")) &&
            evidence_text.contains(
                QStringLiteral("authorized=authorized-value")) &&
            evidence_text.contains(QStringLiteral("parent=<absent>")) &&
            evidence_text.contains(QStringLiteral("term=xterm-256color")) &&
            evidence_text.contains(QStringLiteral("colorterm=truecolor")),
        "child must observe only the explicit final environment and exact cwd");
    ok &= check(
        process_count_for_image(fixture->child_image) == 0U,
        "controlled child must be gone after the exit fact is reconciled");
    return ok;
}

bool path_absent_and_empty_are_not_inherited()
{
    bool ok = true;
    for (const std::optional<std::string> path_value : {
             std::optional<std::string>{},
             std::optional<std::string>{std::string{}},
         })
    {
        QTemporaryDir directory;
        const std::optional<Fixture_paths> fixture = create_fixture(directory);
        if (!check(fixture.has_value(), "PATH fixture must be created")) {
            return false;
        }
        std::vector<environment::Environment_entry> base;
        if (path_value) {
            base.push_back({"PATH", *path_value});
        }
        const workspace::Launch_request_result prepared = request_payload(
            "workspace-provider-child.exe",
            *fixture,
            std::move(base));
        Recording_remote_sink sink;
        Immediate_gui_dispatcher dispatcher;
        Recording_transport transport;
        Recording_termination termination;
        workspace::Terminal_worker_runtime runtime(
            provider_configuration(),
            sink,
            dispatcher,
            transport,
            termination);
        const auto initialized = runtime.initialize();

        const auto result = runtime.run(
            prepared.serialized_request,
            workspace::Launch_platform::WINDOWS,
            path_value ? 104U : 103U);
        ok &= check(
            initialized ==
                workspace::Terminal_worker_initialization_result::READY &&
                prepared.status == workspace::Launch_request_status::ACCEPTED &&
                result == workspace::Terminal_worker_run_result::TERMINATED &&
                transport.delivered.size() == 1U &&
                transport.delivered.front().kind ==
                    workspace::Terminal_child_fact_kind::START_FAILED &&
                !transport.delivered.front().native_dispatch_occurred &&
                process_count_for_image(fixture->child_image) == 0U &&
                !QFileInfo::exists(fixture->evidence),
            path_value
                ? "present-empty PATH must not borrow ambient lookup"
                : "absent PATH must not borrow ambient lookup");
    }
    return ok;
}

bool cwd_is_revalidated_immediately_before_native_dispatch()
{
    QTemporaryDir directory;
    const std::optional<Fixture_paths> fixture = create_fixture(directory);
    if (!check(fixture.has_value(), "cwd fixture must be created")) {
        return false;
    }
    const workspace::Launch_request_result prepared = request_payload(
        "workspace-provider-child.exe",
        *fixture,
        launch_environment(*fixture));
    Recording_remote_sink sink;
    Immediate_gui_dispatcher dispatcher;
    Recording_transport transport;
    Recording_termination termination;
    runtime_detail::Terminal_surface_runtime_adapter provider(
        provider_configuration(),
        sink,
        dispatcher,
        termination,
        [fixture] {
            return QDir(fixture->working_directory).removeRecursively()
                ? runtime_detail::Terminal_native_start_admission::RELEASE
                : runtime_detail::Terminal_native_start_admission::CANCEL;
        });
    runtime_detail::Terminal_worker_coordinator runtime(provider, transport);
    const auto initialized = provider.initialize();

    const auto result = runtime.run_preinitialized(
        prepared.serialized_request,
        workspace::Launch_platform::WINDOWS,
        105U);
    return check(
        initialized == workspace::Terminal_worker_initialization_result::READY &&
            prepared.status == workspace::Launch_request_status::ACCEPTED &&
            result == workspace::Terminal_worker_run_result::TERMINATED &&
            transport.delivered.size() == 1U &&
            !transport.delivered.front().native_dispatch_occurred &&
            process_count_for_image(fixture->child_image) == 0U &&
            !QFileInfo::exists(fixture->evidence),
        "cwd disappearance after admission must reject before native dispatch");
}

bool authorized_environment_cannot_change_surface_owned_or_lookup_names()
{
    struct Rejection_case
    {
        std::vector<environment::Environment_entry> base;
        std::vector<environment::Environment_entry> authorized;
        std::string_view description;
    };
    bool ok = true;
    const std::vector<Rejection_case> cases{
        {
            {{"WORKSPACE_COLLISION", "base"}},
            {{"WORKSPACE_COLLISION", "authorized"}},
            "authorized/base collision must be rejected",
        },
        {
            {},
            {{"TERM", "product-value"}},
            "terminal-owned authorized name must be rejected",
        },
        {
            {},
            {{"PATH", "product-value"}},
            "lookup-sensitive authorized name must be rejected",
        },
    };
    std::uint64_t generation = 106U;
    for (const Rejection_case& rejection : cases) {
        QTemporaryDir directory;
        const std::optional<Fixture_paths> fixture = create_fixture(directory);
        if (!check(fixture.has_value(), "rejection fixture must be created")) {
            return false;
        }
        const workspace::Launch_request_result prepared = request_payload(
            utf8(fixture->child_image),
            *fixture,
            rejection.base);
        Recording_remote_sink sink;
        Immediate_gui_dispatcher dispatcher;
        Recording_transport transport;
        Recording_termination termination;
        workspace::Terminal_worker_runtime runtime(
            provider_configuration(),
            sink,
            dispatcher,
            transport,
            termination);
        const auto initialized = runtime.initialize();

        const auto result = runtime.run(
            prepared.serialized_request,
            workspace::Launch_platform::WINDOWS,
            generation++,
            {},
            rejection.authorized);
        ok &= check(
            initialized ==
                workspace::Terminal_worker_initialization_result::READY &&
                prepared.status == workspace::Launch_request_status::ACCEPTED &&
                result == workspace::Terminal_worker_run_result::TERMINATED &&
                transport.delivered.size() == 1U &&
                transport.delivered.front().kind ==
                    workspace::Terminal_child_fact_kind::START_FAILED &&
                !transport.delivered.front().native_dispatch_occurred &&
                process_count_for_image(fixture->child_image) == 0U,
            rejection.description);
    }
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) ==
        "--workspace-provider-child")
    {
        return run_child(QString::fromUtf8(argv[2]));
    }

    QGuiApplication application(argc, argv);
    bool ok = true;
    ok &= remote_runtime_value_controls_and_teardown_are_owned();
    ok &= pre_native_cancel_allocates_no_backend_or_pid();
    ok &= released_start_dispatches_once_with_exact_environment();
    ok &= path_absent_and_empty_are_not_inherited();
    ok &= cwd_is_revalidated_immediately_before_native_dispatch();
    ok &= authorized_environment_cannot_change_surface_owned_or_lookup_names();
    return ok ? 0 : 1;
}
