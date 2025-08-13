# Creator Tool VST

A JUCE-based VST3 plugin that records incoming audio to timestamped WAV files, with a simple UI to Record/Stop, preview the last recording, and choose a destination folder.

Planned: companion macOS helper app for screen capture, launched from the plugin UI (IPC-based control). For DAW/plugin sandbox reasons, screen recording is handled outside the plugin.

## Build (macOS)

Prereqs:
- CMake 3.21+
- Xcode or a Clang toolchain
- Git (for JUCE FetchContent)

Steps:

```bash
cmake -S . -B build -G Xcode
cmake --build build --config Release
```

The built VST3 will be in:
- `build/CreatorToolVST_artefacts/Release/VST3/Creator Tool VST.vst3`

Copy or symlink it to your VST3 path if needed:
- `~/Library/Audio/Plug-Ins/VST3/`

## Usage

- Insert the plugin on an audio track with the signal you want to record
- Click "Choose Folder" to set the destination
- Click "Record" to start writing a WAV file
- Click "Stop" to finish
- Click "Preview Last" to open the file with the default player

## Notes

- The plugin is pass-through: it does not alter audio.
- Screen recording will be implemented via a separate helper app using ScreenCaptureKit/AVFoundation, controlled via IPC.
