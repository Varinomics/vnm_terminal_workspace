#include <vnm_terminal_workspace/terminal_owner_client.h>
#include <vnm_terminal_workspace/terminal_worker_composition.h>
#include <vnm_terminal_workspace/terminal_worker_envelope.h>
#include <vnm_terminal_workspace/terminal_worker_runtime.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>

#include <algorithm>
#include <concepts>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace workspace = vnm::terminal_workspace;

namespace {

struct Consumer_product_configuration
{
    std::string locator;
};

struct Consumer_product_policy
{
    using Configuration = Consumer_product_configuration;

    inline static constexpr std::string_view package_id =
        "consumer.terminal_worker";
    inline static constexpr std::string_view family_id = "consumer";
    inline static constexpr std::string_view configuration_schema =
        "Consumer_terminal_worker_configuration@1";
    inline static constexpr std::array capabilities{
        workspace::Terminal_worker_package_capability::REMOTE_UI,
        workspace::Terminal_worker_package_capability::WHOLE_MESSAGE_INPUT,
    };
    inline static constexpr std::array<std::string_view, 1>
        product_environment_names{"CONSUMER_CONTROL_TOKEN"};

    static std::optional<std::string> encode_configuration(
        const Configuration& value) noexcept
    {
        if (value.locator != "consumer-locator") {
            return std::nullopt;
        }
        return std::string{"{\"locator\":\"consumer-locator\"}"};
    }

    static std::optional<Configuration> decode_configuration(
        std::string_view value) noexcept
    {
        return value == "{\"locator\":\"consumer-locator\"}"
            ? std::optional<Configuration>(
                Configuration{"consumer-locator"})
            : std::nullopt;
    }

    static void clear_configuration(Configuration& value) noexcept
    {
        std::fill(value.locator.begin(), value.locator.end(), '\0');
        value.locator.clear();
    }
};

static_assert(workspace::Fixed_terminal_worker_package_policy<
    Consumer_product_policy>);

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
    std::optional<workspace::Terminal_worker_output_capture_configuration>,
    Consumer_sink&,
    Consumer_dispatcher&,
    Consumer_transport&,
    Consumer_termination&,
    workspace::Terminal_worker_complete_settings_sink&,
    workspace::Terminal_worker_diagnostic_observation_sink&>);

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    Consumer_sink sink;
    Consumer_dispatcher dispatcher;
    Consumer_transport transport;
    Consumer_termination termination;
    workspace::Terminal_worker_noop_complete_settings_sink settings_sink;
    workspace::Terminal_worker_noop_diagnostic_observation_sink diagnostic_sink;
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
        std::nullopt,
        sink,
        dispatcher,
        transport,
        termination,
        settings_sink,
        diagnostic_sink);

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
    workspace::Terminal_worker_envelope envelope;
    envelope.provider_namespace = "consumer.provider";
    envelope.serialized_request = prepared.serialized_request;
    envelope.platform = workspace::Launch_platform::WINDOWS;
    const workspace::Terminal_worker_envelope_result encoded =
        workspace::encode_terminal_worker_envelope(envelope);
    const workspace::Terminal_worker_envelope_result decoded =
        workspace::decode_terminal_worker_envelope(
            encoded.serialized_envelope);
    const auto fixed =
        workspace::encode_terminal_worker_fixed_parameters<
            Consumer_product_policy>(
                envelope,
                Consumer_product_configuration{"consumer-locator"});
    const auto decoded_fixed =
        workspace::decode_terminal_worker_fixed_parameters<
            Consumer_product_policy>(fixed.serialized_parameters);
    [[maybe_unused]] const auto owner_service_entry =
        &workspace::run_terminal_owner_service<Consumer_product_policy>;
    using Product_client =
        workspace::Terminal_owner_package_client<Consumer_product_policy>;
    static_assert(!Public_surface_escape<Product_client>);
    [[maybe_unused]] const auto compile_typed_launch = [](
        Product_client& client,
        std::span<const std::uint8_t> request_bytes) {
        workspace::Terminal_worker_launch_configuration launch;
        launch.surface.title = "Typed consumer launch";
        return client.new_launch(
            request_bytes,
            workspace::Launch_platform::WINDOWS,
            std::move(launch),
            Consumer_product_configuration{"consumer-locator"},
            std::vector<vnm::environment_policy::Environment_entry>{
                {"CONSUMER_CONTROL_TOKEN", "consumer-token"},
            });
    };
    const workspace::Terminal_worker_initialization_result initialization =
        runtime.initialize();
    if (!prepared.request || !decoded.envelope || !decoded_fixed.parameters ||
        decoded.envelope->serialized_request != prepared.serialized_request ||
        decoded_fixed.parameters->configuration.locator !=
            "consumer-locator" ||
        initialization !=
            workspace::Terminal_worker_initialization_result::READY)
    {
        std::fprintf(
            stderr,
            "consumer setup failed: prepared=%d envelope_error=%d decoded=%d "
            "request_equal=%d initialization=%d\n",
            prepared.request.has_value(),
            static_cast<int>(encoded.error),
            decoded.envelope.has_value(),
            decoded.envelope &&
                decoded.envelope->serialized_request ==
                    prepared.serialized_request,
            static_cast<int>(initialization));
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
    const workspace::Terminal_worker_message_submission_result submission =
        runtime.submit_message(std::span<const std::uint8_t>{});
    const bool forwarded = runtime.forward_state(resize) &&
        runtime.request_present();
    runtime.shutdown();
    const bool success =
        result == workspace::Terminal_worker_run_result::TERMINATED &&
            !owner_configuration.scope.product_identity.empty() &&
            submission.outcome ==
                workspace::Terminal_worker_message_submission_outcome::
                    EMPTY_MESSAGE &&
            forwarded && termination.terminated && sink.created == 1 &&
            sink.destroyed == 1;
    if (!success) {
        std::fprintf(
            stderr,
            "consumer run failed: result=%d submission=%d forwarded=%d "
            "terminated=%d created=%d destroyed=%d\n",
            static_cast<int>(result),
            static_cast<int>(submission.outcome),
            forwarded,
            termination.terminated,
            sink.created,
            sink.destroyed);
    }
    return success ? 0 : 1;
}
