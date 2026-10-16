# OpenNR

OpenNR is a source-only C++20 toolchain that reconstructs the pinned
DLSS Neural Rendering compatibility DLLs from a user-supplied
NVIDIA DLL. It never loads the input DLL and does not modify a game
installation.

## Support boundary

The default profile, `compat-fp16-310.9.16`, supports NVIDIA DLSS-NR
**310.9.16.0**:

- Input size: **266,424,944 bytes**
- Input SHA-256: `2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F`
- Reference output SHA-256: `CDFA6C1EC784867182164C7836F324F087B5C9F374D0C9FE46F54999FEABF3ED`

The legacy profile, `compat-fp16`, supports NVIDIA DLSS-NR **310.8.0.0**:

- Input size: **165,840,496 bytes**
- Input SHA-256: `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`
- Reference output SHA-256: `E67DEE209320CDAFE0E93E45675D7AA34323A53ACC57A72B2E40A181581C989A`

Each profile accepts only its pinned input. Other DLL versions are rejected.
Native inspection can inventory another PE,
but supporting it requires a separately audited recipe and transformation
review. Both reference outputs have been reproduced exactly by the C++ builder.
The 310.9.16 output has been reported working in-game, but comprehensive runtime
correctness, image-quality, compatibility, and performance validation remains
outstanding. The compatibility host patches target D3D11/D3D12; CUDA/Vulkan
backend compatibility is not claimed.

OpenNR does **not** distribute NVIDIA DLLs, PTX, cubins, model weights, SDK
headers, or generated compatibility DLLs.

## Requirements

- Windows x64
- Visual Studio 2022 with the C++ desktop toolchain
- CMake 3.23 or newer
- The CUDA tools pinned by the selected profile in `profiles/`

| Profile | CUDA toolkit | `ptxas` / `fatbinary` | `cuobjdump` | PTX |
| --- | --- | --- | --- | --- |
| `compat-fp16-310.9.16` (default) | 13.4 | 13.4.92 | 13.4.92 | 9.4 |
| `compat-fp16` (legacy) | 13.3 | 13.3.33 | 13.3.29 | 9.3 |

The new golden was reproduced with the CUDA 13.4.3 redistributable tools.
CUDA 13.3 cannot assemble the new profile's PTX 9.4 sources.

CUDA is used only for offline device-code assembly and fatbinary construction.
The default build does not execute GPU workloads.
The profile is the single source of truth for the toolkit version and exact
`ptxas`, `fatbinary`, and `cuobjdump` versions required for golden reproduction.

## Setup

Place a copy of the supported DLL at:

`bin/nvngx_dlssnr.310.9.16.dll`

Alternatively, supply its existing location with `--input "<path-to-DLL>"`.
The input is read-only; generated outputs stay under the repository's `build/`
directory unless `--output` is supplied. No game installation is modified.

For the legacy profile, use `bin/nvngx_dlssnr.dll` and pass
`--profile compat-fp16` to each command.

The `bin/` directory ignores all content except its `.gitignore`; the input DLL
must never be committed.

If CUDA is installed outside its default location, pass
`--cuda-bin <directory>` to `openNR.cmd` after the command name. This directory
must contain `ptxas.exe` and `fatbinary.exe`. If `cuobjdump.exe` is in a separate
directory, also pass `--cuobjdump-bin <directory>`; otherwise it is looked up in
the same directory. No repository-local CUDA installation is required.

## Commands

### Verify the input

`openNR.cmd verify-base`

Requires the selected profile's exact input identity. For example, use
`openNR.cmd verify-base --profile compat-fp16` to verify the legacy input.

### Inspect a PE

`openNR.cmd inspect [DLL] [--output FILE]`

Inventories an AMD64 PE32+ DLL without loading it. The report includes section
bounds and SHA-256 values plus input/reference identities recognized by the
selected profile.

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
`build/compat-fp16-310.9.16/nvngx_dlssnr.dll`. Output directories must be new to prevent
stale artifacts from entering acceptance.

Use `build.cmd --profile compat-fp16` to reproduce the legacy golden at
`build/compat-fp16/nvngx_dlssnr.dll`. Both runners forward `--input`, `--output`,
`--cuda-bin`, and `--cuobjdump-bin` to the builder.

The build:

1. Validates the input DLL and PE structure.
2. Independently discovers all 15 original fatbins.
3. Extracts original SM120 PTX and cubins.
4. Generates SM75, SM86, and SM89 compatibility PTX.
5. Compiles and verifies all 45 compatibility cubins.
6. Packs all four architectures, preserving original SM120 cubins byte-for-byte.
	The new profile relocates seven expanded fatbins into an appended read-only
	`.nrcubin` PE section and updates checked host references and sizes; utility
	fatbins remain in their original slots. The legacy profile uses original slots.
7. Applies checked host compatibility patches.
8. Publishes only when the complete DLL matches the selected reference SHA-256.

### Run native tests

`test.cmd`

Tests borrowed text views, PTX operation contracts, PE parsing, bounded fatbin
discovery, appended PE section validation, GPU acquire-fence lowering, process
failures, hashing, and transformation failure paths.

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
