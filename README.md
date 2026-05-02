# vray_viewport_viewer - minimal standalone viewer for V-Ray shared-memory images

## Build
```
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
  cmake --build build --config Release
```
  GLFW is downloaded and built automatically by CMake (FetchContent).
  No other dependencies required.

## Setup
  Viewport IPR  : works out of the box - just start rendering in Blender

  VFB IPR       : set VRAY_BLENDER_PROGRESSIVE_UPDATES=2 before launching Blender

## Run
  vray_viewport_viewer.exe           (auto-detects VRayZmqServer.exe)

  vray_viewport_viewer.exe <pid>     (explicit ZMQ server PID)
