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
request/session identities, and pre-custody cancellation. Products apply
their fixed product-owned reserved-name policy before request preparation;
capability issuance, binding, and trusted environment contributions remain
outside this target, and no product-name list travels with a launch.

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

`vnm_terminal_workspace::vnm_terminal_workspace_surface_provider` is the
only installed worker-runtime composition. Its PIMPL owns the real framework
remote-UI runtime, a fixed packaged neutral QML root, exactly one
`VNM_TerminalSurface`, the reusable scrollbar, internal layout/focus/timestamp
connections, and the neutral coordinator. The installed API accepts only value
configuration and host-neutral remote-sink, GUI-dispatch, child-fact, and
termination controls; it exports no Qt object, root, surface, provider adapter,
or native-start handle. It owns settings application, search, settings-window
and shortcut mechanics, row timestamps, optional bounded output capture,
value-only whole-message submission, a complete-settings sink, and the closed
output-activity/first-text-frame/backend-error observation sink. Initialization
establishes the hosted surface before a one-shot run projects the strict request
plus any already-authorized environment entries into the sole structured start
API. The composition bounds fact reconciliation and terminates an unreconciled
worker so hosted cleanup can settle it. It does not authorize capabilities, own
session custody, persist history, or define product projection.
Source builds may set `VNM_TERMINAL_SOURCE_DIR`,
`VNM_TERMINAL_SURFACE_SOURCE_DIR`, and `VNM_QML_CHROME_SOURCE_DIR`; installed
consumers resolve the published `vnm_terminal` and surface packages.

`vnm_terminal_workspace::vnm_terminal_workspace_owner_client` is the installed
value-only boundary to the packaged shared terminal-owner service. Owner locks,
endpoints, one-time invitations, and exact authorized viewer identities are
scoped by product and application instance. New-launch is the sole custody and
worker-start operation. Attach-existing accepts an already-owned session,
framework generation, authority epoch, and current attachment revision; it
does not prepare or start another worker, surface, or child. Input and state
forwarding require the current live attachment of a running generation.
Value-only whole-message submission uses those same authenticated caller,
generation, running-custody, and attachment-revision gates.

The owner is the sole terminal custody and child-fact reconciler. It records
only framework-supplied nonzero generations, converges readiness and close
causes once, and exposes an atomic live-custody plus neutral settlement-receipt
snapshot. An optional lifetime capability may keep a protected custody
attachable and receive its settlement; otherwise viewer disconnect closes,
settles, drains, and purges the private bounded receipt inbox at shutdown.
Neither the owner nor client selects product recovery policy, persists
history, issues capabilities, or owns product projection.

The owner-to-worker handoff uses the strict
`vnm_terminal_workspace_envelope@1` value codec. The neutral package target is
fixed to package ID `vnm_terminal_workspace.terminal_worker`, family
`vnm_terminal_workspace`, capability `remote_ui`, and an empty product
environment allowlist. Product worker targets own their separate typed wrapper,
manifest, capability policy, and compile-time environment allowlist; none is a
launch-time workspace registry. The installed fixed-package composition binds
those compile-time values to the shared owner client/service and combines the
common envelope with only the selected policy's closed typed configuration
record in framework `params_json`; it exposes no generic product payload.
