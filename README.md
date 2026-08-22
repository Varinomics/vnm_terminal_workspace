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
request/session identities, and pre-custody cancellation. The independently
encoded capability contribution carries only a trusted, exact-name-scoped
environment contribution. Optional contribution failures remain local;
required contribution failures reject the binding.

The capability allowlist is serialized in ascending unsigned UTF-8 byte order.
Platform name-equivalent duplicates and Windows case collisions reject before
ordering, serialization canonicalizes valid typed input, and strict decode
rejects a noncanonical wire order. A product-owned name may be both an
additional reserved base input and an allowed contribution name; that exact
intersection is the only reserved-name exception. Framework, infrastructure,
terminal-owned, lookup-sensitive, and pseudo-variable names remain prohibited.

This target performs no executable resolution, final-environment composition,
native admission, process launch, generation allocation, surface construction,
or terminal-child custody. It links only the framework environment-policy
target to verify that the explicit base is already sanitized. Callers must not
log serialized payloads, argument values, or environment values.

For source integration, set `VNM_FRAMEWORK_SOURCE_DIR` to a current framework
checkout or provide `vnm_framework::vnm_environment_policy` before adding this
project. The network fallback tracks the owned framework `master` branch.
