# City Builder

Direct3D 12 prototype for a city-builder game. The current executable is a graphics
smoke test: it opens a window and renders a rotating, depth-tested cube with a small
ambient and directional-light shader.

## Layout

The engine package layout is prepared under `Engine/` (`Core`, `Platform`, `Renderer`,
`ECS`, `Assets`, `Physics`, `Audio`, `Scripting`, and `Game`). Runtime entry code is in
`src/` and HLSL sources are in `shaders/`.

## Running

Build the two C++ files with a Windows C++ toolchain, the Windows SDK, and the Direct3D
12 libraries. Run the executable either from the repository root or from `src/`; the
renderer resolves the shader source directory from both locations.
