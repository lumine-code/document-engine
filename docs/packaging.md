# Packaging and release

`@lumine-code/document-engine` is delivered as a Git dependency and builds one N-API addon during installation; it is not published to the npm registry.

## Superstring ABI boundary

Document Engine consumes only Superstring's C/POD `SnapshotLease` header and runtime type-tagged lease object. Its `abi_version` matches the Superstring package major, while `struct_size` permits compatible field additions within that major. It must never include Superstring C++ classes, STL types, parser code, grammar code, marker ownership, or link a second copy of Superstring.

`script/resolve-superstring-abi.js` locates and validates the producer header in this order: `SUPERSTRING_SOURCE_PATH`, `SUPERSTRING_PATH`, a flat sibling checkout, the CI-only nested checkout fallback, and an installed scoped dependency. The build fails before compilation when no compatible header is present.

The final Git dependency must declare an exact `github:lumine-code/superstring#<sha>` dependency. This edge is required even when Lumine also pins Superstring: npm must know that the header producer is installed before Document Engine's `install` lifecycle runs. Add the dependency only after the Superstring ABI commit has been pushed, and let `npm install` regenerate the lockfile.

## Native dependency matrix

| Component          | Source or artifact                | Verification                                                |
| ------------------ | --------------------------------- | ----------------------------------------------------------- |
| Tree-sitter 0.27.0 | Pinned source archive             | SHA-256 and bundled MIT license                             |
| PCRE2 10.44        | Pinned source archive             | SHA-256 and bundled BSD license                             |
| Wasmtime 48.0.1    | Official C API archive per target | SHA-256 and bundled Apache-2.0-with-LLVM-exceptions license |

Wasmtime artifacts are pinned for `win32-x64`, `win32-arm64`, `linux-x64`, `linux-arm64`, `darwin-x64`, and `darwin-arm64`. Native builds use `npm_config_arch` when cross-compiling and otherwise use the running Node architecture. The ordinary CI matrix compiles on Windows, Linux, and macOS; release qualification must additionally compile and run the smoke suite on every architecture not represented by the hosted matrix.

The linker flags mirror the official [Wasmtime C API linking guide](https://docs.wasmtime.dev/c-api/index.html): pthread, dl and math on Linux; no extra macOS libraries; and `ws2_32`, `advapi32`, `userenv`, `ntdll`, `shell32`, `ole32` and `bcrypt` on Windows.

The addon targets Node 24 or newer and uses N-API rather than the V8 ABI. `NAPI_VERSION` comes from node-gyp's `napi_build_version`; teardown behavior requires the Node 24 generation used by Lumine and Electron.

## Install and archive contents

The `install` lifecycle provisions verified dependencies, compiles the addon, copies `build/Release/document-engine.node` aside, removes compiler intermediates and `.native-deps`, then restores only the loadable addon. Cleanup validates every destructive target as a child of the package directory.

Lumine runs `electron-rebuild` after dependency installation, which provisions those inputs a second time. Its `postbuild` therefore invokes the same package-owned cleanup, its packager refuses a dirty Document Engine build tree, and its file rules also exclude `.native-deps` plus compiler/linker products as defense in depth. The shipped package directory may contain `build/Release/document-engine.node`, but no build cache or intermediate beside it.

`npm run check:package` validates the canonical description, native dependency matrix, license files, ABI header, archive contents, and source-package size without publishing anything. `npm run check:release` additionally refuses a release until the exact Superstring Git pin exists.

The source archive intentionally includes build sources, bindings, runtime JavaScript and TypeScript declarations, provisioning scripts, notices, and documentation. It excludes tests, CI configuration, build output, dependency caches, downloaded archives, and `node_modules`.

## CI topology

CI checks out repositories as siblings so module and fixture resolution matches the permanent flat Lumine workspace. Unit and package checks run on Windows, Linux, and macOS with Node 24. A representative Linux integration job runs the complete integration suite and differentially checks JavaScript, Python, IPython, HTML, GFM, Rust, TypeScript, and Vue plus the TODO injection grammar. A Linux ASan/UBSan job exercises the addon boundary, and the scheduled full-fleet job checks out all language repositories at the exact SHAs recorded in the package catalogue before compiling and executing every query.

Cross-repository checkout refs must be replaced with exact verified commits before the first release. Tracking remote heads is acceptable only while bootstrapping the new repository and must not become the release contract.

## Release sequence

1. Commit and push the Superstring `SnapshotLease` producer, its lifecycle tests, and packaged public header; record the resulting 40-character commit SHA.
2. Add that exact Superstring Git dependency to Document Engine and run `npm install` so npm, not a manual edit, regenerates `package-lock.json`.
3. Review newer patch releases in the pinned Wasmtime line, update the artifact/hash matrix if adopted, or record a deliberate hold in `lem/holds.json` before pinning every cross-repository CI checkout to verified compatibility commits.
4. Run `npm run check:release`, `npm run build`, unit tests, representative integration tests, the full query fleet, lifecycle/soak tests, and the release benchmark profile.
5. Create and push the Document Engine repository, then set its GitHub About text to the canonical description from `package.json` and `README.md`.
6. Add an exact Document Engine Git pin to Lumine with `npm install`, validate the bundled dependency graph, and advance the Superstring pin to the same producer commit.
7. Teach `lem` about the new repository edge and add the architecture, troubleshooting, and performance-gate documentation to `lumine-code.github.io`.

Do not add Document Engine to `packages/index.json`: it is a native library pinned by the editor, not an installable editor package. Do not run `npm publish`.
