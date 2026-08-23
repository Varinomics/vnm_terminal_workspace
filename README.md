# vnm_terminal_workspace

`vnm_terminal_workspace` owns neutral terminal-workspace integration above
`vnm_terminal_surface` and below product policy. Its pure request target is
available as:

```cmake
find_package(vnm_terminal_workspace CONFIG REQUIRED)
target_link_libraries(app PRIVATE
    vnm_terminal_workspace::vnm_terminal_workspace_request)
```

The request contract preserves separated argument bytes, an absolute working
directory, a complete caller-sanitized explicit base environment, stable
request/session identities, and pre-custody cancellation. Callers may inject
additional product-owned reserved names so those names cannot enter the base;
capability issuance, binding, and trusted environment contributions remain
outside this target.

Request preparation and decoding are deterministic and never inspect the
filesystem. `probe_terminal_working_directory` is a separate optional advisory;
the terminal surface revalidates the working directory at native admission.

This target performs no executable resolution, final-environment composition,
native admission, process launch, generation allocation, surface construction,
terminal-child custody, or product capability policy. It links only the
framework environment-policy target to verify that the explicit base is already
sanitized. Callers must not log serialized payloads, argument values, or
environment values.

For source integration, set `VNM_FRAMEWORK_SOURCE_DIR` to a current framework
checkout or provide `vnm_framework::vnm_environment_policy` before adding this
project. The network fallback tracks the owned framework `master` branch.
