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
| Pure terminal launch request and cwd advisory | `src/terminal_launch_request.cpp`, `include/vnm_terminal_workspace/terminal_launch_request.h` | `vnm_terminal_workspace_request_tests` |
| Neutral settlement receipt inbox | `src/terminal_settlement_receipt_inbox.{h,cpp}` | `vnm_terminal_workspace_settlement_receipt_tests` |
