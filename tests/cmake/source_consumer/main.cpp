#include <vnm_terminal_workspace/terminal_launch_request.h>

#include <utility>

int main()
{
    vnm::terminal_workspace::Terminal_launch_request request;
    request.launch_request_id = "source-consumer-request";
    request.session_id = "source-consumer-session";
    request.argv = {"shell"};
    request.working_directory = "/workspace";
    request.base_environment_complete = true;
    request.cancellation.identity = "source-consumer-cancellation";
    const auto advisory =
        vnm::terminal_workspace::probe_terminal_working_directory(
            request.working_directory,
            {});
    const auto result =
        vnm::terminal_workspace::prepare_terminal_launch_request(
            std::move(request),
            vnm::terminal_workspace::Launch_platform::POSIX);
    return advisory ==
                vnm::terminal_workspace::Working_directory_advisory::NOT_PROBED &&
            result.status ==
                vnm::terminal_workspace::Launch_request_status::ACCEPTED
        ? 0
        : 1;
}
