# Firefox engine patches

The `firefox/` gitlink points at an unmodified upstream ref. The two patches in
this directory contain the ruyijs-specific SpiderMonkey changes. They are kept
out of the submodule so that the ref stays clean; apply them after
`git submodule update` and before building the donor shell.

Both patches are generated from the exact gitlink recorded by this branch and
apply to a clean checkout of `firefox/`.

| Patch | Contents | Depends on |
| --- | --- | --- |
| `01-proxy.patch` | WindowProxy glue (`RuyijsWindowProxy.{cpp,h}`) and the matching `moz.build` entry. Required for `window === globalThis` and `document.defaultView === window`. | nothing |
| `02-trace.patch` | Standalone call/JSVMP/EVAL/WASM trace, JIT/VM/WASM hooks, and the matching `moz.build` entries. | nothing (independent of `01-proxy.patch`) |

The two patches touch `js/src/vm/moz.build` at different anchors
(`RuntimeFuses.cpp` and `Stack.cpp`), so they can be applied in any order.

## Apply

```sh
git submodule update --init firefox
cd firefox
git apply ../patches/01-proxy.patch
git apply ../patches/02-trace.patch
cd ..
```

Then build the donor shell and the ruyijs runtime as described in the project
build documentation. Without `01-proxy.patch`, the host code in
`src/runtime/evaluation.cpp` fails to link at `js::ruyijs_window_proxy::*`.
Without `02-trace.patch`, the host trace calls compiled through
`src/runtime/standalone_trace.h` become no-ops.

## Regenerate

After changing the patched Firefox working tree:

```sh
bash ci/generate_engine_patches.sh
```

This reads the current `firefox/` working tree and rewrites both patch files
against its current `HEAD`, keeping the proxy and trace changes separated.
