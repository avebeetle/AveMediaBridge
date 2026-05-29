# AveMediaBridge Agent Instructions

- Keep changes small and reviewable.
- Do not commit build artifacts, FFmpeg binaries, generated output, or credentials.
- Do not expose FFmpeg types in public DLL ABI.
- Do not pass C++ classes, `std::string`, `std::vector`, or ownership-bearing types across DLL boundary.
- Preserve AveMediaBridgeLabApp smoke commands unless a task explicitly changes them.
- Run Release build and relevant smoke tests before commit.
- Commit only files relevant to the task.
- Push to main only when explicitly requested.
