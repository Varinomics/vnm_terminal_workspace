#include "vnm_terminal_workspace/terminal_worker_composition.h"

int main(int argc, char** argv)
{
    return vnm::terminal_workspace::run_terminal_owner_service<
        vnm::terminal_workspace::Neutral_terminal_worker_package_policy>(
            argc,
            argv);
}
