# Repository Guidelines

## Project Structure & Module Organization
`src/` contains the plugin implementation: `Plugin.cpp` holds the D3D12 hooks and Unity lifecycle code, `Plugin.h` exposes the native entry points, and `HookWrapper.hpp` wraps MinHook calls. `PluginAPI/` vendors Unity native plugin headers; treat it as external API surface, not project logic. `External/minhook/` is the MinHook submodule. Local IDE and build outputs live in `.vs/`, `.idea/`, and `cmake-build-*`; keep generated files out of commits.

## Build, Test, and Development Commands
- `git submodule update --init --recursive` - fetch MinHook before the first build.
- `cmake -S . -B build` - configure a Visual Studio build tree for the `UnityBindless` DLL.
- `cmake --build build --config RelWithDebInfo` - compile the plugin; output lands in `build/bin/RelWithDebInfo/`.
- `cmake --build build --config Debug` - build a debug DLL for stepping through hooks in Visual Studio.

## Coding Style & Naming Conventions
Use C++20 and keep MSVC `/W4` warnings clean. Follow the existing style: 4-space indentation, opening braces on the same line, and standard library types fully qualified. Match the current naming patterns: exported functions in PascalCase (`CheckBindlessSupport`), constants with a `k` prefix (`kMainDescriptorHeapMinCount`), globals with `g_`, file-static state with `s_`, and member fields with `m_`. Prefer small helper functions over repeating hook or logging logic.

## Testing Guidelines
There is no automated test target in this repository yet. For every change, at minimum:
- configure and build both `Debug` and `RelWithDebInfo`;
- verify the plugin loads in a Unity project running the D3D12 backend;
- smoke-test exported APIs such as `WarmupPlugin`, `CheckBindlessSupport`, and descriptor creation paths.

Document manual validation steps in the PR when you touch hook installation, descriptor heap selection, or profiler integration.

## Commit & Pull Request Guidelines
Recent history uses short, imperative subjects, sometimes with prefixes like `fix:` or `update:`. Keep commit titles concise and specific, for example `fix: guard descriptor heap capture`. Pull requests should explain the Unity/D3D12 scenario, summarize behavior changes, list validation steps, and include logs or screenshots when editor-visible behavior changes. Link related issues when available.

## Integration Notes
This plugin is Windows/D3D12-focused. Avoid changing vendored code in `PluginAPI/` or `External/minhook/` unless you are intentionally updating a dependency.
