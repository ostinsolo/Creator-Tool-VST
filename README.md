# Creator Tool VST

A JUCE-based VST3 plugin that records your DAW’s audio and macOS screen, producing perfectly synced A+V captures (MOV), with options for audio-only WAV and screen-only MOV.

## Features

- Audio-only recorder
  - Timestamped WAV files written via a lock-free FIFO + ThreadedWriter (no disk I/O on audio thread)
- Screen recording (macOS)
  - ScreenCaptureKit (macOS 12.3+) preferred; AVFoundation fallback
  - Default target: main display (window targeting planned)
  - Resolution menu: Auto, 720p, 1080p, 1440p, 2160p
- Combined A+V capture (single-writer)
  - One AVAssetWriter session writes video and audio for perfect sync
  - Hardware H.264 via VideoToolbox through AVAssetWriter
  - Realtime, low-latency design; audio offloaded to a background drain thread
- Destination folder chooser and “Preview Last” button
- Robust logging (Desktop/CreatorTool_Logs) for diagnosis

## Build (macOS)

Prerequisites:
- CMake 3.21+
- Xcode 15+ (or modern Clang toolchain)
- JUCE as a local submodule at `external/JUCE`

Setup:
```bash
cmake -S . -B build -G Xcode
cmake --build build --config Release -- -jobs 4
```
The VST3 is produced at:
- `build/CreatorToolVST_artefacts/Release/VST3/Creator Tool VST.vst3`

It is also copied to:
- `~/Library/Audio/Plug-Ins/VST3/`

## Usage

- Insert the plugin on your master (or any) track in the DAW
- Choose a destination folder
- Optional: pick a video resolution
- Audio-only:
  - Record → Stop → WAV written
- Screen-only:
  - Screen Rec → Screen Stop → MOV written
- A+V combined:
  - Record A+V → Stop A+V → MOV written (with synced audio)

Filenames include millisecond timestamps to avoid overwrites.

## Permissions (macOS)

- Screen Recording permission will be required for ScreenCaptureKit/AVFoundation
- Grant in System Settings → Privacy & Security → Screen Recording (for the host/DAW)

## Implementation details

- Plugin UI/Logic: `src/PluginEditor.*`, `src/PluginProcessor.*`
- Audio-only recorder: `src/AudioRecorder.*`
  - Uses `AbstractFifo` + `ThreadedWriter`
- macOS screen capture: `src/ScreenRecorder.mm/.h`
  - Prefers ScreenCaptureKit (SCStream) with AVAssetWriter for H.264 video
  - Fallback to AVFoundation movie file recording
  - Combined A+V path writes audio as 16-bit PCM using a ring buffer drained by a low-priority thread
  - Audio timestamps align to the first video PTS for perfect sync
- Logging: `src/Logging.h` (Desktop/CreatorTool_Logs)

## Performance and audio stability

- Audio thread safety:
  - No disk I/O; no heap allocations during processing; lock-free FIFOs used
- Video pipeline:
  - Uses ScreenCaptureKit with backpressure tolerance
  - Resolution can be reduced to lower GPU/CPU load
- Encoder settings:
  - AVAssetWriter H.264 video (realtime)
  - 16-bit PCM audio (interleaved)
- Threads:
  - Audio drain thread (background priority)
  - SCK sample handler is on its own queue

If you hear glitches during A+V recording:
- Reduce capture resolution
- Avoid heavy UI/graphical activity while capturing
- Check logs in `~/Desktop/CreatorTool_Logs` for warnings/errors

## Roadmap

- Window-only capture as default (DAW window) with ScreenCaptureKit content filters
- MP4 export path (AAC audio) using `AVAssetWriter` or a fast post-export
- VideoToolbox explicit configuration (bitrate, profile, keyframe interval)
- Windows support (DXGI Desktop Duplication + Media Foundation H.264 MFT)
- In-plugin preview of latest A+V capture

## License

This project depends on JUCE (AGPLv3/Commercial). Ensure your usage complies with JUCE licensing.
