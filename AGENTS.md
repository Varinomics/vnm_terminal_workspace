# AGENTS

## Common Varinomics rules

This repository follows the shared Varinomics standards. Do not duplicate,
reinterpret, or weaken those rules locally.

Before modifying code, read:

- `varinomics_coding_style_guideline.md`
- `varinomics_coding_style_llm_addendum.md`
- `varinomics_review_scope.md`
- `varinomics_change_governance.md`

The standards live at `C:\plms\varinomics\varinomics-standards`. The LLM
Windows toolchain policy in that repository and the user-wide agent
instructions govern every configure, build, and test command.

Do not stage or commit transient review reports, plans, working notes, or
investigation artifacts unless the user explicitly requests a repository
artifact.

## Product map

| Subsystem | Owning source | Focused test target |
|---|---|---|
| Pure terminal launch request, strict worker envelope, fixed package composition, and cwd advisory | `src/terminal_{launch_request,worker_envelope,worker_composition}.cpp`, `include/vnm_terminal_workspace/terminal_{launch_request,worker_envelope,worker_composition}.h` | `vnm_terminal_workspace_request_tests` |
| Neutral settlement receipt inbox | `src/terminal_settlement_receipt_inbox.{h,cpp}` | `vnm_terminal_workspace_settlement_receipt_tests` |
| Neutral terminal worker runtime, real surface provider, and child facts | `src/terminal_{worker_runtime,surface_runtime_provider}.cpp`, `include/vnm_terminal_workspace/terminal_worker_runtime.h` | `vnm_terminal_workspace_worker_runtime_tests`, `vnm_terminal_workspace_surface_provider_tests` |
| Terminal custody, fact reconciliation, hosted ownership, and settlement integration | `src/terminal_{owner_core,hosted_owner,owner_proxy_gate}.{h,cpp}`, `include/vnm_terminal_workspace/terminal_owner_host.h` | `vnm_terminal_workspace_owner_core_tests`, `vnm_terminal_workspace_hosted_owner_tests` |
| Scoped shared owner service, invitation/client, attach-existing proxy, and value-only message route | `src/terminal_owner_{service_main,service_executable_main,client,wire,process_identity}.cpp`, `include/vnm_terminal_workspace/terminal_owner_client.h` | `vnm_terminal_workspace_owner_client_tests` |
