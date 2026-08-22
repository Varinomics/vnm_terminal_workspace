#include <vnm_terminal_workspace/terminal_launch_request.h>

#include <utility>

int main()
{
    vnm::terminal_workspace::Terminal_launch_request request;
    request.launch_request_id = "install-consumer-request";
    request.session_id = "install-consumer-session";
    request.argv = {"shell"};
    request.working_directory = "/workspace";
    request.base_environment_complete = true;
    request.cancellation.identity = "install-consumer-cancellation";
    const auto result =
        vnm::terminal_workspace::prepare_terminal_launch_request(
            std::move(request),
            vnm::terminal_workspace::Launch_platform::POSIX);
    return result.status ==
        vnm::terminal_workspace::Launch_request_status::ACCEPTED
        ? 0
        : 1;
}
