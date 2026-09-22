# Contributing

## Scope

The default `compat-fp16` profile must continue to reproduce the pinned golden
DLL exactly. Experimental algorithms or support for another NVIDIA DLL belong
in separate profiles and must not weaken golden validation.

Do not commit NVIDIA DLLs, extracted PTX, cubins, model weights, SDK snapshots,
captures, generated outputs, or proprietary runtime components.

## Native development

Requirements are listed in `README.md`. Use the `native` CMake preset in Visual
Studio Code or run `test.cmd` from an x64 Visual Studio developer environment.
The native unit tests do not require an NVIDIA DLL or GPU.

Before submitting a change:

1. Build the `native` CMake preset in Release mode.
2. Run the `native` CTest preset or `test.cmd`.
3. Run clang-format in check mode on changed C++ files.
4. Check the patch for whitespace errors.
5. For transformation or packing changes, perform a fresh build using a
   supported DLL and confirm all cubin hashes and final DLL identity.

Do not publish proprietary inputs or attach generated DLLs to issues or pull
requests.

## Transformation rules

- Express replacements as named architectural or PTX operations.
- Use regex only for lexical discovery, not as an implicit semantic schema.
- Validate known operand counts and register banks.
- Reject recognized but unsupported forms explicitly.
- Keep DLL-specific data in the audited recipe rather than module-index branches.
- Treat the final DLL hash as authoritative and cubin hashes as localization
  diagnostics. Generated PTX text hashes are non-blocking.

## Reports

Include compiler/tool versions and focused test results. Runtime reports must
also identify GPU, driver, inputs, warmup policy, and correctness results.
Performance claims without correctness acceptance are not actionable.
