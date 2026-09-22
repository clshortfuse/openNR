# OpenNR

OpenNR is a source-only C++20 toolchain that reconstructs the pinned
`compat-fp16` DLSS Neural Rendering compatibility DLL from a user-supplied
NVIDIA DLL. It never loads the input DLL and does not modify a game
installation.

## Support boundary

The current profile supports exactly NVIDIA DLSS-NR **310.8.0.0**:

- Input size: **165,840,496 bytes**
- Input SHA-256: `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`
- Reference output SHA-256: `E67DEE209320CDAFE0E93E45675D7AA34323A53ACC57A72B2E40A181581C989A`

Other DLL versions are rejected. Native inspection can inventory another PE,
but supporting it requires a separately audited recipe and transformation
review. Exact output reproduction has been validated; runtime correctness,
image quality, compatibility, and performance are not yet claimed.

OpenNR does **not** distribute NVIDIA DLLs, PTX, cubins, model weights, SDK
headers, or generated compatibility DLLs.

## Requirements

- Windows x64
- Visual Studio 2022 with the C++ desktop toolchain
- CMake 3.23 or newer
- The CUDA Toolkit version pinned by `profiles/compat-fp16.json`

CUDA is used only for offline device-code assembly and fatbinary construction.
The default build does not execute GPU workloads.
The profile is the single source of truth for the toolkit version and exact
`ptxas`, `fatbinary`, and `cuobjdump` versions required for golden reproduction.

## Setup

Place a copy of the supported DLL at:

`bin/nvngx_dlssnr.dll`

The `bin/` directory ignores all content except its `.gitignore`; the input DLL
must never be committed.

If CUDA is installed outside its default location, pass
`--cuda-bin <directory>` to `openNR.cmd`.

## Commands

### Verify the input

`openNR.cmd verify-base`

Requires the exact supported input identity.

### Inspect a PE

`openNR.cmd inspect [DLL] [--output FILE]`

Inventories an AMD64 PE32+ DLL without loading it. The report includes section
bounds and SHA-256 values plus recognized input/reference identities.

### Extract original device images

`openNR.cmd extract --output <new-directory>`

Validates the input PE, independently discovers bounded fatbins in its single
`.data` section, requires their ranges to agree with the audited recipe, and
extracts validated SM120 PTX and cubins.

### Generate compatibility PTX

`openNR.cmd sources --output <new-directory>`

Reconstructs and lowers source without compiling compatibility cubins.
Generated PTX hashes are diagnostic; compiled cubins and the final DLL are the
artifact acceptance boundaries.

### Build the reference DLL

`build.cmd`

Equivalent to `openNR.cmd build`. The default output is
`build/compat-fp16/nvngx_dlssnr.dll`. Output directories must be new to prevent
stale artifacts from entering acceptance.

The build:

1. Validates the input DLL and PE structure.
2. Independently discovers all 15 original fatbins.
3. Extracts original SM120 PTX and cubins.
4. Generates SM75, SM86, and SM89 compatibility PTX.
5. Compiles and verifies all 45 compatibility cubins.
6. Rebuilds the original fatbin slots and applies checked host patches.
7. Publishes only when the complete DLL matches the reference SHA-256.

### Run native tests

`test.cmd`

Tests borrowed text views, PTX operation contracts, PE parsing, bounded fatbin
discovery, process failures, hashing, and transformation failure paths.

## Benchmarks

Optional benchmark and analysis tools are under `benchmarks/`. They are not
used by the DLL builder. Some analysis tools require Python and NumPy; native
D3D11/D3D12 harnesses require locally obtained SDK dependencies. GPU execution
requires an explicit benchmark command. See `benchmarks/README.md`.

## Repository safety

Generated files belong under ignored `build/`. Local SDK snapshots belong under
ignored `local/`. NVIDIA inputs belong under ignored `bin/`. Existing output
files are never silently replaced with different content.

See `ROADMAP.md` for remaining runtime-validation and portability work.

## License

Original OpenNR source is available under the MIT License. See `LICENSE` and
`THIRD_PARTY_NOTICES.md`. Third-party and user-provided software remains subject
to its own license.
