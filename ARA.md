# ARA integration

## Build

The `ThirdParty/ARA_SDK` submodule is pinned to Celemony's public `releases/2.3.0` tag.
Initialize submodules before configuring:

```sh
git submodule update --init --recursive
cmake -S . -B build -DNEURALNOTE_ARA=ON -DBUILD_UNIT_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`NEURALNOTE_ARA` defaults to `ON`. Set it to `OFF` to build without the ARA SDK.
`NEURALNOTE_ARA_SDK` can point at an existing SDK root containing `ARA_API` and `ARA_Library`.
The plugin advertises ARA API generation 2.0 and supports the playback renderer, editor renderer,
and editor view roles supplied by JUCE. VST3 and AU carry ARA; Standalone keeps its normal behavior.
Existing plugin IDs and ordinary plugin state are retained.

## Workflow

1. Apply NeuralNote as an ARA extension using your DAW's clip/region extension workflow.
2. The first assigned clip is imported automatically as soon as the host makes its audio available.
   Use **Settings > Import host clip (ARA)** to choose another assigned or selected region.
3. Select instruments and click **Transcribe**. The model and preview work as before.
4. Drag or export MIDI and place it at the source clip's start in the DAW.

Import respects the clip's source trim and duration and retains all source channels for the existing
mono transcription downmix. Moving a clip on the DAW timeline retains its transcription; changing its
source trim creates a separate excerpt. Finished raw notes and model choice are stored per excerpt
inside the ARA audio modification archive, including partial object saves and modification clones.
The automatic import after reopening a session also recalls the excerpt's archived result.

Import creates an independent audio snapshot. DAW edits made during transcription cannot change
the model's input. Source sample updates invalidate the cached results for future imports, while an
in-flight job can finish against its existing snapshot. Reimport after editing the source to analyse
the new audio. Menu actions resolve clip IDs again when invoked, so deleted/changed regions are rejected.

Source audio is passed through by the ARA playback renderer, with background read-ahead in real time
and channel/sample-rate conversion. With an imported ARA clip, Play, Stop and seek follow the DAW
transport. The plugin playhead displays time relative to the clip's timeline start; plugin transport
controls request the corresponding host transport operation. Host looping controls playback. The tempo field and MIDI export follow the current host BPM,
including tempo changes while stopped. When host BPM is available the field is read-only; without
host BPM, including in Standalone, export tempo remains manually editable. The ARA
editor renderer does not add another copy of the host audio. The editor fills the host pane and allows
width and height to change independently, with the timeline using the available space.

## Inference resource limits

Transcription uses half the available logical CPU count (rounded down, minimum one thread)
and a low-priority job worker. A process-wide
lock rejects a second simultaneous NeuralNote transcription rather than allocating another model.
Load/inference exceptions become a transcription failure instead of escaping the job thread.
Stages and completed chunks are appended to `<NeuralNote data directory>/transcription-diagnostics.log`.
This limits CPU pressure but does not establish the cause of an operating-system freeze.

## Current boundaries

- The first clip imports automatically; transcription remains explicit. Host selection changes do not
  overwrite an existing take, and Clear stays empty until a manual import.
- No host-requested note analysis/content readers, automatic MIDI-track creation, or time-stretching.
  The factory advertises no analysis content types or playback transformation capabilities.
  Bounce/render time-stretched clips first.
- Audio snapshots are immutable WAVs in `<NeuralNote data directory>/ara-audio`, deduplicated by
  SHA-256. They are retained across clears and session reloads. Import performs host reads on the
  document/message thread, so large clips can briefly block the editor.
- ARA archives contain transcription data, not source audio; the DAW owns the original source.
  On another machine, reimport the host clip to recreate the local waveform/preview snapshot.
- Windows builds and simulated-host tests are verified locally. macOS AU and real DAW behavior
  require the checks below before release. The r4 Windows verification build enables Vulkan GPU inference and disables ASIO. Earlier
  installers r1-r3 were CPU-only. Source-build defaults remain unchanged.

## Validation

With `BUILD_UNIT_TESTS=ON`, `ARAIntegrationTests` drives the real ARA factory through a simulated
host. It checks region enumeration, source trimming, stereo imports, archive save/restore, mono
playback at a different sample rate, stopped transport, source edits, disabled sample access, and
stale menu selections. It also checks automatic import, host Play/Stop/seek, plugin transport
requests with the clip offset, fractional BPM updates while stopped, invalid/missing BPM fallback,
and independent editor resizing to fill an ultrawide host pane. The existing `UnitTests` target continues to cover transcription utilities.

Before distributing, verify in each supported DAW:

- Scan the VST3/AU and attach through the host's ARA extension workflow.
- Attach to a trimmed clip while stopped; confirm automatic import, then transcribe and export MIDI.
- Resize the editor pane horizontally and vertically; confirm that the waveform and piano roll fill it.
- Play, stop, seek and loop the host, including overlapping clips and offline audio export.
- Save/reopen, reimport the excerpt, and confirm that its notes return without retranscribing.
- Duplicate a modification, trim/move/delete clips, disable/re-enable sample access, and replace
  source media; ensure playback recovers and new imports do not reuse stale notes.
- Load the plugin as a normal effect in a non-ARA host and exercise recording/file import.
