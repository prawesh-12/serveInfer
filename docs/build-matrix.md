# Build matrix: which backends this repository can actually contain

Companion to [device-fallback.md](device-fallback.md), which covers what happens when a
backend *faults*. This one covers the question underneath it: which backends a given
build of this repository can compile, load, initialize, execute and release at all.

The two are easy to conflate, and the whole point of A3 and A4 falls apart if you do.
"llama.cpp supports Hexagon upstream" and "this binary can run on a Hexagon DSP" are
different claims, and only the second one lets you say the project supports Qualcomm NPUs.

---

## 0. Version this document describes

Everything below was read out of the tree in `backend/inference-worker/llama-src/`, not
from upstream documentation.

| Field | Value |
|---|---|
| Upstream | `ggml-org/llama.cpp` |
| Commit | `e85caa81ea2b65797396018c179b87ad61fa38ab` |
| Upstream build number | `10582` (`git rev-list --count`) |
| ggml version | `0.21.0` (`llama-src/ggml/CMakeLists.txt:6-9`) |
| Synchronized from | a clean upstream checkout at the same commit |
| Date of upstream commit | 2026-08-22 |

The vendored tree is a byte-exact, unmodified subset of upstream at that commit — all
1538 files verified against `git cat-file blob e85caa81:<path>`. It does **not** include
`docs/` or `CMakePresets.json`, so anything quoted from those is cited to github.com at
that ref rather than to a local path.

### 0.1 Version change record — this was an upgrade, not a resync

This tree was deliberately moved forward. Recording it loudly because a silent llama.cpp
version bump is the kind of thing that invalidates every claim in the rest of this
document.

| | Before | After |
|---|---|---|
| Commit | `255582687b8dd211fdbc582e43ab842491554e94` | `e85caa81ea2b65797396018c179b87ad61fa38ab` |
| Release tag / build | `b9180` | build `10582` |
| ggml version | `0.11.1` | `0.21.0` |
| Commit date | 2026-05-16 | 2026-08-22 |
| Vendored files | 1333 | 1538 |

The two commits are **1402 upstream commits apart**, and the old one is a direct ancestor of
the new one, so the delta is exact rather than estimated. Across the paths this repo vendors
today — `CMakeLists.txt LICENSE cmake include src ggml vendor` — 860 files differ: 305 added,
40 deleted, 515 modified.

Why upgrade at all, given "do not silently upgrade llama.cpp": the requirement that all
required llama.cpp source be synchronized *from the reference tree* and the requirement
that no two llama.cpp commits be mixed cannot both be met while keeping `b9180`, because
the reference tree only contains `e85caa81`. Cherry-picking individual files forward would
be exactly the forbidden mixing. So the tree was moved wholesale to one internally
consistent version, and the change is recorded here and in `CLAUDE.md` rather than being
made quietly.

**API impact on the wrapper: none.** All 41 `llama_*` / `ggml_backend_dev_*` symbols used
by `inferEngine.cpp`, `hardwareProbe.cpp` and the backend/router sources still exist with
byte-identical declarations. Two struct changes are worth knowing even though they do not
affect this project:

- `llama_model_params` **lost** `use_mmap`, `use_direct_io` and `use_mlock`; they are
  replaced by a single `enum llama_load_mode load_mode`. Nothing in `backend/` set those
  fields, and `llama_model_default_params()` returns `LLAMA_LOAD_MODE_AUTO`, which still
  mmaps — so the `/dev/shm` shared-weights design is unaffected.
- `llama_context_params` gained `n_outputs_max`, `n_outputs_max_per_seq` and `ctx_other`.
  `inferEngine.cpp` only sets `n_ctx`, `n_batch` and `n_ubatch`, all unchanged.

### 0.2 What is vendored, and what is not

Selection is by dependency analysis against the actual CMake graph, not by taste.

| Path | Vendored | Why |
|---|---|---|
| `CMakeLists.txt`, `LICENSE` | yes | Entry point; `CMakeLists.txt:198` does `license_add_file("llama.cpp" "LICENSE")` (the function comes from `cmake/license.cmake`), and MIT requires the licence to ship. |
| `cmake/` | yes (13 files) | `build-info.cmake`, `common.cmake`, `license.cmake`, `llama-config.cmake.in`, `llama.pc.in` are all included by the root file. |
| `include/` | yes (2) | `llama.h`, `llama-cpp.h` — the public API the wrapper compiles against. |
| `src/` | yes (215) | The `llama` library target itself. |
| `ggml/` | yes (1279) | ggml core + every backend adapter. Only CPU and CUDA actually compile here; the rest are inert source guarded by `GGML_*` options that default OFF. Kept whole so `metal`/`hexagon` ladder tiers can be enabled without a second sync, and because the subtree's CMake enumerates backends as one unit. |
| `vendor/` | **yes (27) — new** | `add_subdirectory(vendor)` is **unconditional** at `CMakeLists.txt:227` in this version. It was conditional in `b9180`, which is why the old copy could omit it. `vendor/hash` genuinely compiles: `hash.cpp`, `xxhash.c`, `sha1.c`, `sha256.c` appear as real translation units in `compile_commands.json`. |
| `common/` | **no — dropped** | Was vendored before but is not required: `add_subdirectory(common)` is guarded by `LLAMA_BUILD_COMMON`, which this build forces OFF, and `llama` links only `ggml`. Removing it drops 69 files with no build impact. |
| `examples/`, `tools/`, `app/`, `tests/`, `pocs/`, `benches/`, `ci/`, `.github/`, `docs/`, `models/`, `gguf-py/`, `conversion/`, `scripts/`, `media/`, `grammars/`, `licenses/`, `requirements/` | no | Not reachable from the build with `LLAMA_BUILD_{COMMON,TESTS,TOOLS,EXAMPLES,SERVER,UI,APP,MTMD}=OFF`. |

`backend/inference-worker/CMakeLists.txt` now also forces `LLAMA_BUILD_APP=OFF` and
`LLAMA_BUILD_MTMD=OFF`, the two options added since `b9180` that guard `app/` and
`tools/mtmd/` — directories deliberately not vendored.

### 0.3 Build stamp caveat

`llama-src/` has no `.git` of its own, so upstream's git probes walk up and find *this*
repository. `backend/inference-worker/CMakeLists.txt` pins `LLAMA_BUILD_COMMIT=e85caa81`
and `LLAMA_BUILD_NUMBER=10582` before `add_subdirectory`, which is honoured — the `llama`
target compiles with `-DLLAMA_COMMIT="e85caa81"`.

`ggml/CMakeLists.txt:13-29` is not fixable the same way: it re-runs `git rev-parse` itself
and overwrites any value the parent set, so configure still prints
`ggml commit: <this repo's commit>-dirty`. That stamp is cosmetic and wrong; the
authoritative ggml version is the `0.21.0` in the table above, read from
`ggml/CMakeLists.txt:6-9`. It was left alone rather than patched, so that the vendored
tree stays byte-exact and provably unmixed.

---

## 1. The honest three-platform matrix

| Target | Backends in one binary | Blocked here, and why |
|---|---|---|
| **Linux x86-64 + NVIDIA** (what this repo builds today) | `CPU` + `CUDA`, optionally `VULKAN`, `OPENCL`, `RPC`, `BLAS` | **Metal**: `find_library(METAL_FRAMEWORK Metal REQUIRED)` is a hard configure failure off Apple. **Hexagon**: `FATAL_ERROR` without `HEXAGON_SDK_ROOT`, and the DSP half cross-compiles to QuRT. **Core ML / ANE**: not a llama.cpp backend at all. |
| **macOS Apple Silicon** | `CPU` + `METAL`, plus `BLAS`/`ACCELERATE` by default | **CUDA**: no CUDA Toolkit for Apple silicon. **Hexagon**: wrong SoC. **ANE**: needs a separate Core ML adapter above the wrapper, not a ggml backend. |
| **Windows 11 ARM64 Snapdragon** | `CPU` (ARM64) + `HEXAGON` + `OPENCL` (Adreno) | **CUDA**: no NVIDIA GPU. **Metal**: not Apple. Needs clang/LLVM rather than MSVC, two Qualcomm SDKs, driver signing, and a CMake preset this vendored subset does not carry. |

> **One binary cannot hold CUDA, Metal and Hexagon.** Not as a design preference — because of
> `find_library(... REQUIRED)` and `message(FATAL_ERROR ...)` gates in this exact tree.
> Platform-specific builds are the only correct answer, with one platform-independent
> hardware-policy layer above the wrapper.

That layer is what this repository actually ships. Every probe in
[`deviceBackends.cpp`](../backend/inference-worker/deviceBackends.cpp) compiles on every platform and
returns `wrong_platform` off its own OS, and every adapter in
[`inferenceBackend.cpp`](../backend/inference-worker/inferenceBackend.cpp) does the same. So a
`EDGE_DEVICE_LADDER` string written for a Snapdragon laptop still parses, still selects and
still explains itself on this Linux box.

---

## 2. Where the gates actually are

Options live in `llama-src/ggml/CMakeLists.txt`. Each becomes a subdirectory through
`ggml_add_backend(<Name>)` at `ggml/src/CMakeLists.txt:483, 587-603`, which **tests only
whether the option is truthy — it applies no platform gating of its own**
(`ggml/src/CMakeLists.txt:426-437`). All real gating is inside each backend's own CMake,
and it fails hard rather than skipping.

| Option | Declared | Default | Hard dependency | Effective platform |
|---|---|---|---|---|
| `GGML_CPU` | `ggml/CMakeLists.txt:189` | **ON** | none | all |
| `GGML_CUDA` | `:199` | OFF | `find_package(CUDAToolkit)`, `enable_language(CUDA)`; links `cudart_static`, `cublas[Lt]_static`, and `CUDA::cuda_driver` unless `GGML_CUDA_NO_VMM` | Linux/Windows with NVIDIA |
| `GGML_METAL` | `:238` | ON iff `APPLE` (`:95-103`) | `find_library` Foundation, Metal, MetalKit — all `REQUIRED` (`ggml-metal/CMakeLists.txt:1-3`) | macOS/iOS only |
| `GGML_HEXAGON` | `:270` | OFF | Hexagon SDK + Hexagon Tools; `FATAL_ERROR` if `HEXAGON_SDK_ROOT` is unset (`ggml-hexagon/CMakeLists.txt:4-6`); `libcdsprpc` at runtime | Snapdragon aarch64 only |
| `GGML_OPENCL` | `:263` | OFF | `find_package(OpenCL REQUIRED)` **and** `find_package(Python3 REQUIRED)` (`ggml-opencl/CMakeLists.txt:1-2`) | any host with an OpenCL SDK; the Adreno kernels are Adreno-specific |
| `GGML_VULKAN` | `:223` | OFF | Vulkan SDK + `vulkan-shaders-gen` | Linux/Windows/Android |

One more constraint that shapes everything in §3: this project sets
`BUILD_SHARED_LIBS=OFF` (`backend/inference-worker/CMakeLists.txt:51`), which makes
`GGML_BACKEND_DL` impossible — `ggml/src/CMakeLists.txt:188-190` is an explicit
`FATAL_ERROR "GGML_BACKEND_DL requires BUILD_SHARED_LIBS"`. **Every compiled backend is
statically linked and statically registered.** There is no runtime plugin loading and, for
CUDA, no runtime opt-out.

---

## 3. Why CPU-only means a separate process, not a flag

`ggml/src/ggml-backend-reg.cpp:119-122` registers CUDA unconditionally:

```cpp
ggml_backend_registry() {
#ifdef GGML_USE_CUDA
    register_backend(ggml_backend_cuda_reg());
#endif
```

Contrast Vulkan a few lines down at `:129-136`, which honours `GGML_DISABLE_VULKAN`.
**CUDA has no such escape hatch in this version.** And `ggml_backend_cuda_reg()`
(`ggml-cuda.cu:5501`) iterates `ggml_cuda_info().device_count`, which calls
`ggml_cuda_init()` → `cudaGetDeviceCount` and friends. So merely *counting devices*
initializes CUDA.

Nor can it be undone:

| Candidate | What it really does |
|---|---|
| `ggml_backend_free()` | Frees one backend *instance*: streams, cuBLAS handles, pools. Does not touch the device or the registry. |
| `ggml_backend_unload()` | Erases entries from two `std::vector`s (`ggml-backend-reg.cpp:266-289`). Frees nothing on the device. In a static build it would make CUDA *invisible* while the context stayed resident — it hides the leak rather than fixing it. |
| `llama_backend_free()` | `ggml_quantize_free()` and nothing else (`src/llama.cpp:148-150`). |
| `cudaDeviceReset`, `cuDevicePrimaryCtxRelease`, `cuCtxDestroy` | Do not appear anywhere in `llama-src/ggml/src/`. |

The registry destructor says so itself (`ggml-backend-reg.cpp:176-178`):

```cpp
~ggml_backend_registry() {
    // FIXME: backends cannot be safely unloaded without a function to destroy all the backend resources,
    // since backend threads may still be running and accessing resources from the dynamic library
```

**Therefore: the only way to guarantee a process holds no CUDA context is for that process
never to initialize CUDA.** That is a decision that has to be made before the process
starts, which is why it lives in the supervisor and travels as `CUDA_VISIBLE_DEVICES=-1`
in the child's environment (`backend/hardware/capacityPlan.cpp`, `workerBackendEnv`). The
variable is read by the NVIDIA driver itself, below ggml: with no visible device,
`ggml_cuda_init` sees `device_count == 0`, `ggml_backend_cuda_reg` registers zero devices,
and no primary context is ever created.

Reference: [CUDA environment variables](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/environment-variables.html)
— an invalid index truncates the visible-device list, so index `-1` yields none.

The same fact is why hardware discovery runs in a short-lived
`edge-inference-worker --probe-hardware` child rather than in the supervisor. Asking ggml
how much VRAM is free costs the asking process a CUDA context for its lifetime; a process
that exits immediately afterwards is the one place that cost is acceptable, and process
exit is the only complete teardown on offer.

**Packaging note.** `ggml/src/ggml-cuda/CMakeLists.txt:179-183` links `CUDA::cuda_driver`
(i.e. `libcuda.so.1`) as a hard link-time dependency unless `GGML_CUDA_NO_VMM=ON`. A
CUDA-enabled worker binary will therefore not start on a machine with no NVIDIA driver.
Ship two binaries, or set `GGML_CUDA_NO_VMM=ON`.

---

## 4. Qualcomm Hexagon on Windows ARM64

### What is actually in the tree

The backend is fully vendored at `llama-src/ggml/src/ggml-hexagon/`: `ggml-hexagon.cpp`
(4510 lines), `htp-drv.cpp`, and an `htp/` directory of 73 files — 27 DSP kernel sources
and 43 headers, including HMX matmul and HMX flash-attention. It is real and actively
developed. Its public header exposes exactly three symbols: `ggml_backend_hexagon_init`, `ggml_backend_is_hexagon`,
`ggml_backend_hexagon_reg`.

### Why this repository cannot build it

`-DGGML_HEXAGON=ON` on this machine dies before any compiler runs
(`ggml-hexagon/CMakeLists.txt:4-6`):

```cmake
if (NOT IS_DIRECTORY "${HEXAGON_SDK_ROOT}")
    message(FATAL_ERROR "Make sure HEXAGON_SDK_ROOT point to the correct Hexagon SDK installation.")
endif()
```

Even with the SDK, `build_htp_skel(v73 v75 v79 v81)` (lines 79-82) drives an
`ExternalProject_Add` against a toolchain file that sets `CMAKE_SYSTEM_NAME QURT` and
`CMAKE_SYSTEM_PROCESSOR Hexagon` (`htp/cmake-toolchain.cmake:8-9`). The host half needs an
ARM64 target too: `htp-drv.cpp:322-341` loads `libcdsprpc.dll` / `libcdsprpc.so` at
runtime and resolves `rpcmem_alloc`, `dspqueue_*`, `remote_handle64_*` — FastRPC symbols that exist only on
Snapdragon.

### Maturity, stated by upstream at this exact commit

* In-tree and verifiable locally — `ggml-hexagon.cpp:4291` logs
  `"ggml-hex: Hexagon backend (experimental) : allocating new registry"`.
* Upstream's own backend table marks it **`[In Progress]`**:
  [README.md at e85caa81](https://github.com/ggml-org/llama.cpp/blob/e85caa81ea2b65797396018c179b87ad61fa38ab/README.md).
  Only Hexagon and OpenVINO carry that marker; CUDA, Metal and OpenCL carry none.

### What a Windows ARM64 Snapdragon build would require

From [docs/backend/snapdragon/windows.md at e85caa81](https://github.com/ggml-org/llama.cpp/blob/e85caa81ea2b65797396018c179b87ad61fa38ab/docs/backend/snapdragon/windows.md)
and [docs/backend/snapdragon/README.md at e85caa81](https://github.com/ggml-org/llama.cpp/blob/e85caa81ea2b65797396018c179b87ad61fa38ab/docs/backend/snapdragon/README.md):

* LLVM core libraries + Clang, CMake, Git and Python, *and* Visual Studio 2026 with the
  MSVC arm64 standard and runtime libraries — the compiler is clang, but the MSVC runtime
  is still needed
* **Qualcomm Adreno OpenCL SDK v2.3.2** → `OPENCL_SDK_ROOT`
* **Qualcomm Hexagon SDK Community Edition v6.6.0.0** → `HEXAGON_SDK_ROOT`
* **Hexagon Tools 19.0.07**, shipped inside the SDK → `HEXAGON_TOOLS_ROOT`
* **Windows SDK 10.0.26100.0** → `WINDOWS_SDK_BIN`
* **`HEXAGON_HTP_CERT`**, a self-signed `.pfx`; the HTP skels are catalogued and signed with
  `inf2cat` and `signtool` (`ggml-hexagon/CMakeLists.txt:87-113`, targeting `/os:10_25H2_ARM64`)
* Test-signing enabled via `bcdedit`, with the certificate imported into both *Trusted Root
  Certification Authorities* and *Trusted Publishers*
* `cmake --preset arm64-windows-snapdragon-release` — **`CMakePresets.json` is not vendored
  here**, so its flags would have to be brought in or reproduced by hand
* The cross-compile toolchain files that *are* vendored: `llama-src/cmake/arm64-windows-llvm.cmake`,
  `arm64-linux-clang.cmake`, `arm64-apple-clang.cmake`

### What this repository ships instead

[`QualcommHexagonBackend`](../backend/inference-worker/inferenceBackend.h) — an adapter at the
hardware-policy boundary that probes honestly (`QnnHtp.dll` and `dxcore.dll` on Windows,
`wrong_platform` elsewhere), translates QNN status families through `faultFromQnnStatus`,
and carries a **build-gated route** to the llama/ggml execution path. The state machine
around it — availability, `ERROR_DEVICE_REMOVED`, unhealthy, fallback, health check,
recovery — is complete and tested through a fake.

#### The route, and the gate on it

`QualcommHexagonBackend::execute()` does not contain an unconditional refusal any more. It
asks `hexagonCompiledIn()`, and when that is true and an `InferEngine` is bound it calls
`runThroughEngine()` — the same function `LlamaInferenceBackend::execute()` calls for the
CUDA and CPU tiers. There is no second execution path and no stub.

The gate is ggml's own macro, not one invented for this project:

| Where | What it does |
| --- | --- |
| `llama-src/ggml/src/CMakeLists.txt:426-437` (`ggml_add_backend`) | with `-DGGML_HEXAGON=ON` and `GGML_BACKEND_DL=OFF`, runs `target_compile_definitions(ggml PUBLIC GGML_USE_HEXAGON)` |
| `llama-src/ggml/src/CMakeLists.txt:601` | `ggml_add_backend(Hexagon)` is the call site |
| `llama-src/ggml/src/ggml-backend-reg.cpp:61,153` | `#ifdef GGML_USE_HEXAGON` → `register_backend(ggml_backend_hexagon_reg())` |
| `llama-src/src/CMakeLists.txt:63` | `target_link_libraries(llama PUBLIC ggml)`, so the PUBLIC definition reaches `inferenceBackend.cpp` |

These lines were verified identical against upstream at `e85caa81` when the copy was taken,
so the vendored tree is byte-exact on the gate. The lines above did move
across the 1402-commit upgrade — `ggml_add_backend` used to sit at `:295-306` — but the
mechanism did not change, which is the part the route depends on.

#### What has and has not been verified

Verified locally, and reproducible:

* the routed branch **compiles in every build**, gated or not — only the predicate is behind
  the `#if`, so the route cannot rot (`hexagonRouteTests.cpp`, in `make test`)
* compiling `inferenceBackend.cpp` with `-DGGML_USE_HEXAGON` flips
  `hexagonCompiledIn()` to `true`, makes `executable()` true once an engine is bound, and makes
  `execute()` route into the engine instead of refusing
* without the macro — every build this repository has ever produced —
  `hexagonCompiledIn()` is `false`, `executable()` is `false`, and `execute()` returns
  `qualcomm hexagon backend is not compiled into this build (GGML_HEXAGON=OFF, …)`

**Never verified, and not claimed anywhere:** that inference has run on a Hexagon DSP. The
routed branch has never been linked against a real `ggml-hexagon`, because it cannot be built
on this host. Reaching a state where it could actually execute needs all of:

1. **A Snapdragon Windows ARM64 (or Android aarch64) target.** `ggml-hexagon` cross-compiles
   its DSP half to QuRT; there is no x86-64 Linux configuration of it.
2. **`HEXAGON_SDK_ROOT` and `HEXAGON_TOOLS_ROOT`** pointing at a Qualcomm Hexagon SDK
   (Community Edition v6.6.0.0) and Hexagon Tools 19.0.07, or
   `ggml-hexagon/CMakeLists.txt:4-6` is a hard `FATAL_ERROR`.
3. **`-DGGML_HEXAGON=ON` with `GGML_BACKEND_DL=OFF`**, or the macro is never defined and the
   backend is loaded dynamically instead, which this gate does not cover.
4. **`libcdsprpc` at runtime**, plus the signed `libggml-htp.inf` driver package.
5. **Two changes in `worker.cpp`**, which is outside this adapter: the engine must be bound
   to the adapter at its registration site (`Worker::selectDevice`, currently
   `std::make_shared<QualcommHexagonBackend>()` with no engine), and the engine must stop
   being built with `forceCpu = (active tier != "cuda")`, which would otherwise load the model
   with `n_gpu_layers = 0` and run the whole graph on the CPU while the router reported `npu`.
   That last point is also why `"npu"` is deliberately **not** in `tierIsLlamaServed()`.

Until all five hold, the honest statement is: **the route exists, is compiled, and is
build-gated shut. It is unreachable and unverified on this platform.**

#### No equivalent for the ANE

`CoreMlAneBackend` deliberately has no matching gate. Hexagon has a real upstream macro to
test; Core ML has none, because there is no ggml Core ML backend at all (see §5 — zero matches
tree-wide). Writing an `#ifdef` there would mean inventing a symbol nothing defines.

---

## 5. Apple Metal and the Neural Engine

### They are not the same execution resource

`grep -rniE "coreml|core ml|neural engine|MLModel|MLCompute|ANECompiler" llama-src/` returns
**nothing across all 1538 files**. `ggml-metal/CMakeLists.txt:1-3` requires Foundation,
Metal and MetalKit — no CoreML, no ANE framework. `llama_supports_gpu_offload()` returning
true on Apple silicon means *Metal GPU*, nothing more.

The ANE is selected by setting `MLModelConfiguration.computeUnits` on an `MLModel`
([MLComputeUnits](https://developer.apple.com/documentation/coreml/mlcomputeunits)):
`.cpuAndNeuralEngine` allows the CPU and the neural engine but not the GPU; `.cpuAndGPU`
allows both of those "but not the neural engine". That is an API surface entirely disjoint
from llama.cpp, which is why the ANE story lives **above** the wrapper as
[`CoreMlAneBackend`](../backend/inference-worker/inferenceBackend.h) rather than as a ggml backend.

### `kCMErrorUnsupportedOperation`, recorded accurately

A4 names this constant. Three facts about it, kept in the code as well as here:

1. **`kCMError*` is CoreMedia**, an audio/video framework using `CoreMediaErrorDomain` and
   OSStatus values in the −12xxx range. Core ML is a different framework: it reports
   `NSError`s in `MLModelErrorDomain`
   ([MLModelErrorDomain](https://developer.apple.com/documentation/coreml/mlmodelerrordomain)),
   and [`MLModelError.Code`](https://developer.apple.com/documentation/coreml/mlmodelerror-swift.struct/code)
   has **no `unsupportedOperation` case**.
2. **The identifier `kCMErrorUnsupportedOperation` does not appear in any Apple SDK header.**
3. Its nearest real analogue is CoreMedia's **`kCMBaseObjectError_UnsupportedOperation = -12782`**
   (`CMBaseObject.h`), meaning "this object does not implement this method".

So the repo keeps the name A4 uses and gives it the real value:
`constexpr long kCMErrorUnsupportedOperation = -12782` in
[`inferenceBackend.h`](../backend/inference-worker/inferenceBackend.h), with the discrepancy written
down beside it. `faultFromCoreMediaStatus` maps the whole −12xxx family to `kUnsupportedOp`,
so −12782 lands where it should.

Worth stating plainly, because it changes what an ANE fallback is *for*: in practice an
ANE-unsupported layer usually does not surface as an error at all. Core ML partitions the
graph at compile time and silently assigns unsupported layers to the GPU or CPU; the
prediction succeeds, only slower. Detecting "the ANE did not take it" reliably needs
`MLComputePlan` or an Instruments trace, not an error code. The error path this repo
implements is the case where Core ML genuinely refuses.

---

## 6. What "supported" means in this repository

| Claim | True here? |
|---|---|
| CPU inference through llama.cpp | yes, built and running |
| CUDA inference through llama.cpp | **built, not exercised**. The CUDA build compiles clean on Linux + NVIDIA and `--probe-hardware` runs, but no request has been served on a GPU here: the card on this machine sits in a `[GPU requires reset]` state where `cuInit` returns `CUDA_ERROR_NO_DEVICE`, so discovery reports `gpus=0` |
| CUDA capacity planning and per-worker isolation | yes, and the isolation is enforced before `execvp` |
| Full in-process CUDA teardown | **no, and it is not possible at this llama.cpp version** — see section 3. The worker releases everything releasable and then exits to be respawned |
| Metal inference | not built here; would build on macOS with `GGML_METAL=ON` |
| Hexagon / Qualcomm NPU inference | **no**. Vendored upstream, `[In Progress]`, unbuildable on this host, two proprietary SDKs and driver signing away |
| Core ML / ANE inference | **no**. Not a llama.cpp capability at all; the adapter exists, a compiled MLModel does not |
| The fallback state machine for all of the above | yes, complete and tested through injectable fakes |

Everything in the "no" rows is tested as *policy* — the runtime does the right thing when a
backend reports those vendor codes — and is never presented as hardware validation. No test
in this repository has run on a Snapdragon NPU or an Apple Neural Engine.
