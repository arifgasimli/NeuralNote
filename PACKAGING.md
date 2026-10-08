# Package NeuralNote for macOS

- Build the app, VST3 and AU in Release mode (LTO is on by default, `-DLTO=ON`).
  - muscriptor.cpp does not support universal binaries, so a build contains only one architecture (the host's by
    default), and there is one installer per architecture. On Apple Silicon, build the Intel one in a second build
    directory with `-DCMAKE_OSX_ARCHITECTURES=x86_64`, and package it the same way.
- Install [Packages](http://s.sudre.free.fr/Software/Packages/about.html) if you don't have it already.
- Set up an Apple Developer certificate and load it into Keychain (for both the app and the installer).
- Run the `sign_and_package_neuralnote_macos.sh` script to sign the 3 artifacts and package them into an installer
  (.pkg file).
    - Run the script with the path to the release directory containing the Standalone, VST3 and AU directory (usually
      `cmake-build-release/NeuralNote_artefacts/Release`).
      ```bash
      ./sign_and_package_neuralnote_macos.sh cmake-build-release/NeuralNote_artefacts/Release
      ```
    - The script will ask for the Apple ID and password (app specific) for the signing process.
    - An optional second argument names the installer, e.g. `NeuralNote_Installer_Mac_x64.pkg`. The script refuses
      a name for the wrong architecture.
    - The installer will be located in `Installers/Mac/build`, named `NeuralNote_Installer_Mac_arm64.pkg` or
      `NeuralNote_Installer_Mac_x64.pkg` unless named by the second argument.

# Package NeuralNote for Windows

On Windows, NeuralNote is not code signed for now. To create the installer, the following steps are required:

- Build the app and VST3 in Release mode.
- Install [Inno Setup](https://jrsoftware.org/isinfo.php) if you don't have it already.
- Build the installer.
    - In command prompt, from the trunk of the repository, run the following command:
      ```commandline
      "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" Installers\Windows\neuralnote.iss /DReleaseDir="cmake-build-release/NeuralNote_artefacts/Release"
      ```
      `DReleaseDir` should indicate the path to the release directory containing the Standalone and VST3
      directories.

The installer will be located in `Installers/Windows/Output`.

For the ARA development build, use the ARA-enabled Release directory and label the installer:

```powershell
& ISCC.exe /DReleaseDir=build-msvc2/NeuralNote_artefacts/Release /DInstallerName=NeuralNote_ARA_Installer_Windows_x64 /DInstallerInfoFile=ara-readme.txt Installers/Windows/neuralnote.iss
```

Use your installed or portable Inno Setup compiler (`ISCC.exe`).
The installer offers the standalone application and VST3 independently; models download separately.
This development build is unsigned and has ASIO disabled. The r4 and later installers include Vulkan GPU
inference with CPU fallback; r1-r3 were CPU-only builds.

For an isolated packaging smoke test, compile a separate installer with
`/DValidationRoot=<absolute workspace directory>` and a different `InstallerName`.
This redirects both components to that directory and disables shortcuts, legacy-folder cleanup,
uninstall registration, and elevation. Never distribute the validation installer.

For a Windows ARA installer with GPU support, obtain the [official Vulkan SDK](https://vulkan.lunarg.com/sdk/home/),
set `VULKAN_SDK` to the SDK root (headers, import library, glslc and SPIRV-Headers CMake config),
and configure from a Visual Studio developer environment:

```powershell
cmake -S . -B build-msvc2 -DMUSCRIPTOR_VULKAN=ON -DNEURALNOTE_ARA=ON -DBUILD_UNIT_TESTS=ON
cmake --build build-msvc2 --target NeuralNote_VST3 NeuralNote_Standalone ARAIntegrationTests UnitTests
ctest --test-dir build-msvc2 --output-on-failure
$env:NEURALNOTE_INFERENCE_SMOKE = '1'
$env:NEURALNOTE_INFERENCE_DEVICE = 'NVIDIA GeForce RTX 4090'
$env:NEURALNOTE_INFERENCE_MODEL = 'small'
./build-msvc2/Tests/ARAIntegrationTests.exe
```

Use an actual enumerated device name for the explicit-device smoke test, then repeat with `large`
if that model is installed. Check `transcription-diagnostics.log` for the selected **Vulkan** device
and successful inference before packaging. The GPU test is optional in normal CTest because it
requires both hardware and a downloaded model. The Windows driver supplies the Vulkan runtime;
the installer embeds the compute shaders and does not require the SDK on the target machine.

The GPU verification build used official Vulkan SDK 1.4.363.0. Set `VULKAN_SDK` to your SDK
root when rebuilding locally.
