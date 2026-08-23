#include <vnm_terminal_workspace/terminal_owner_client.h>
#include <vnm_terminal_workspace/terminal_worker_runtime.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>

#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace workspace = vnm::terminal_workspace;

namespace {

class Consumer_dispatcher final : public workspace::Terminal_worker_gui_dispatcher
{
public:
    bool dispatch(
        workspace::Terminal_gui_dispatch_kind kind,
        const std::function<void()>& function) override
    {
        if (kind == workspace::Terminal_gui_dispatch_kind::BLOCKING) {
            function();
            return true;
        }
        return QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            function,
            Qt::QueuedConnection);
    }
};

class Consumer_sink final : public workspace::Terminal_worker_remote_sink
{
public:
    std::optional<workspace::Terminal_remote_surface_identity> create_surface(
        const workspace::Terminal_remote_surface_descriptor&) override
    {
        ++created;
        return 1U;
    }

    std::optional<workspace::Terminal_remote_frame_buffer> begin_frame(
        workspace::Terminal_remote_surface_identity,
        std::int32_t width,
        std::int32_t height) override
    {
        const std::int32_t stride = width * 4;
        frame.resize(
            static_cast<std::size_t>(stride) *
            static_cast<std::size_t>(height));
        return workspace::Terminal_remote_frame_buffer{frame, stride};
    }

    void end_frame(
        workspace::Terminal_remote_surface_identity,
        std::span<const workspace::Terminal_remote_damage_rectangle>,
        std::uint32_t) override
    {}

    void destroy_surface(
        workspace::Terminal_remote_surface_identity) override
    {
        ++destroyed;
    }

    void set_cursor(
        workspace::Terminal_remote_surface_identity,
        std::int32_t) override
    {}

    workspace::Terminal_remote_initial_state initial_state() override
    {
        return {};
    }

    std::vector<std::uint8_t> frame;
    int created = 0;
    int destroyed = 0;
};

class Consumer_transport final : public workspace::Terminal_child_fact_transport
{
public:
    workspace::Terminal_child_fact_delivery_result deliver(
        const workspace::Terminal_child_fact&) override
    {
        return {
            workspace::Terminal_child_fact_delivery_status::DELIVERED,
            workspace::Terminal_child_fact_acknowledgement::ACCEPTED,
        };
    }
};

class Consumer_termination final : public workspace::Terminal_worker_termination
{
public:
    void terminate_hosted_worker() override
    {
        terminated = true;
    }

    bool terminated = false;
};

} // namespace

template<typename Type>
concept Public_start_capable = requires(Type& value) {
    value.start_terminal();
};

template<typename Type>
concept Public_surface_escape = requires(Type& value) {
    value.terminal_surface();
};

template<typename Type>
concept Public_scrollbar_escape = requires(Type& value) {
    value.terminal_scrollbar_item();
};

template<typename Type>
concept Public_root_escape = requires(Type& value) {
    value.root_item();
};

template<typename Type>
concept Public_after_load_escape = requires(Type& value) {
    value.after_load();
};

static_assert(!Public_start_capable<workspace::Terminal_worker_runtime>);
static_assert(!Public_surface_escape<workspace::Terminal_worker_runtime>);
static_assert(!Public_scrollbar_escape<workspace::Terminal_worker_runtime>);
static_assert(!Public_root_escape<workspace::Terminal_worker_runtime>);
static_assert(!Public_after_load_escape<workspace::Terminal_worker_runtime>);
static_assert(!Public_surface_escape<workspace::Terminal_owner_client>);
static_assert(!Public_scrollbar_escape<workspace::Terminal_owner_client>);
static_assert(!Public_root_escape<workspace::Terminal_owner_client>);
static_assert(!Public_after_load_escape<workspace::Terminal_owner_client>);
static_assert(std::is_constructible_v<
    workspace::Terminal_worker_runtime,
    workspace::Terminal_worker_surface_configuration,
    Consumer_sink&,
    Consumer_dispatcher&,
    Consumer_transport&,
    Consumer_termination&>);

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    Consumer_sink sink;
    Consumer_dispatcher dispatcher;
    Consumer_transport transport;
    Consumer_termination termination;
    workspace::Terminal_worker_surface_configuration configuration;
    configuration.title = "Installed value-only consumer";
    workspace::Terminal_owner_client_configuration owner_configuration;
    owner_configuration.scope.product_identity = "consumer";
    owner_configuration.scope.application_instance_identity = "compile-only";
    owner_configuration.owner_executable_path = "C:/owner.exe";
    owner_configuration.owner.hosted_worker_host_executable_path =
        "C:/host.exe";
    owner_configuration.owner.terminal_worker_library_path = "C:/worker.dll";
    owner_configuration.owner.provider_namespace = "consumer.provider";
    workspace::Terminal_worker_runtime runtime(
        std::move(configuration),
        sink,
        dispatcher,
        transport,
        termination);

    workspace::Terminal_launch_request request;
    request.launch_request_id = "consumer-request";
    request.session_id = "consumer-session";
    request.argv = {"C:/not-dispatched.exe"};
    request.working_directory = "C:/";
    request.base_environment_complete = true;
    request.cancellation.identity = "consumer-cancellation";
    const workspace::Launch_request_result prepared =
        workspace::prepare_terminal_launch_request(
            std::move(request),
            workspace::Launch_platform::WINDOWS);
    if (!prepared.request ||
        runtime.initialize() !=
            workspace::Terminal_worker_initialization_result::READY)
    {
        return 1;
    }

    workspace::Terminal_remote_state_message resize;
    resize.state_type = 1U;
    resize.width = 640;
    resize.height = 480;
    const workspace::Terminal_worker_run_result result = runtime.run(
        prepared.serialized_request,
        workspace::Launch_platform::WINDOWS,
        1U);
    const bool forwarded = runtime.forward_state(resize) &&
        runtime.request_present();
    runtime.shutdown();
    return result == workspace::Terminal_worker_run_result::TERMINATED &&
            !owner_configuration.scope.product_identity.empty() &&
            forwarded && termination.terminated && sink.created == 1 &&
            sink.destroyed == 1
        ? 0
        : 1;
}
