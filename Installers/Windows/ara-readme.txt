NeuralNote 2.0.0 - ARA development build for Windows x64

This installer contains the ARA-enabled VST3 plugin and the standalone app.
Choose either or both components. Close your DAW before installing.

The VST3 installs to the standard shared VST3 folder:
C:\Program Files\Common Files\VST3\NeuralNote.vst3
The standalone installs to C:\Program Files\NeuralNote.
This build updates an existing NeuralNote installation using the same plugin ID.

ARA usage:
Apply NeuralNote through your DAW's ARA / region-extension workflow.
The first assigned clip imports automatically. Choose instruments and click
Transcribe. Settings > Import host clip (ARA) lets you switch or refresh clips.
Play, Stop and seek follow the DAW, with plugin time relative to the clip start.
The editor fills the host pane with independently resizable width and height.
Export MIDI and place it at the original clip's start. Finished transcriptions
are saved in the ARA session archive and restored on import, including autoimport.
Bounce time-stretched clips before importing.

Download a transcription model separately using the Model button in the app.
Audio processing stays local to your computer.

This build includes Vulkan GPU inference with CPU fallback and does not include ASIO.
It is unsigned. Automated ARA tests pass; real DAW validation is still pending.
See ARA-Usage.md beside the installed standalone app for more details.

Revision 3: transcription is capped at two CPU compute threads, with a low-priority
job worker and one active transcription per DAW process. Inference exceptions
are reported as failures. Diagnostics are written to
%APPDATA%\NeuralNote\transcription-diagnostics.log.
A real Small-model inference on autoimported ARA audio passed locally.
The reported complete system freeze has not been reproduced or conclusively diagnosed.

Revision 4: Vulkan GPU support is included. After fully restarting your DAW,
Settings > Compute device lists compatible GPUs. Auto prefers a discrete GPU;
select NVIDIA GeForce RTX 4090 explicitly to use that card. SDK installation
is not required; the graphics driver must provide Vulkan. The r3 CPU thread
limit and single-transcription guard are retained.
Real Small and Large model inference on an autoimported test ARA clip passed
with explicit NVIDIA GeForce RTX 4090 selection using the Vulkan backend.

Revision 5: plugin/export tempo follows the host project BPM, including changes
while stopped. The tempo field is read-only while following the host; manual
export tempo remains available when the host provides no BPM or in Standalone.
CPU inference uses half the available logical processors (minimum one thread),
so a 32-logical-processor system uses 16 compute threads. Vulkan GPU inference,
the low-priority job worker and the single-transcription guard are retained.
