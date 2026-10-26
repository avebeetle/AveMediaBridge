# AveMediaBridge Agent Instructions

- Keep changes small and reviewable.
- Continue development in the `AveMediaBridge` working folder on `main`. Do not create sibling working copies unless explicitly requested.
- Preserve the accepted AveVoice DLL and FFmpeg set recorded in `tools/runtime/accepted-runtime-20261027.json`; see `docs/WorkingRuntime.md`.
- Do not replace the deployed AveVoice DLL or FFmpeg set without a new explicit user request. Local builds and working-folder cleanup do not authorize deployment.
- Do not commit build artifacts, FFmpeg binaries, generated output, or credentials.
- Do not expose FFmpeg types in public DLL ABI.
- Do not pass C++ classes, `std::string`, `std::vector`, or ownership-bearing types across DLL boundary.
- Preserve AveMediaBridgeLabApp smoke commands unless a task explicitly changes them.
- Run Release build and relevant smoke tests before commit.
- Commit only files relevant to the task.
- Push to main only when explicitly requested.
