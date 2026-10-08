#!/usr/bin/env bash
set -euo pipefail
release=$1
arch=$2
label=$3
out=build-mac/packages
payload=build-mac/pkg-root
mkdir -p "$out" "$payload/Applications" "$payload/Library/Audio/Plug-Ins/Components" "$payload/Library/Audio/Plug-Ins/VST3" "$payload/Library/Application Support/NeuralNote"
for bundle in "$release/Standalone/NeuralNote.app" "$release/AU/NeuralNote.component" "$release/VST3/NeuralNote.vst3"; do
  test -f "$bundle/Contents/Resources/default.metallib"
  test "$(lipo -archs "$bundle/Contents/MacOS/NeuralNote")" = "$arch"
  codesign --force --deep --sign - "$bundle"
  codesign --verify --deep --strict "$bundle"
done
ditto "$release/Standalone/NeuralNote.app" "$payload/Applications/NeuralNote.app"
ditto "$release/AU/NeuralNote.component" "$payload/Library/Audio/Plug-Ins/Components/NeuralNote.component"
ditto "$release/VST3/NeuralNote.vst3" "$payload/Library/Audio/Plug-Ins/VST3/NeuralNote.vst3"
cp ARA.md "$payload/Library/Application Support/NeuralNote/ARA-Usage.md"
cp LICENSE "$payload/Library/Application Support/NeuralNote/LICENSE.txt"
cp Installers/license.txt "$payload/Library/Application Support/NeuralNote/THIRD-PARTY-NOTICES.txt"
cat > "$payload/Library/Application Support/NeuralNote/README.txt" <<'EOF'
NeuralNote ARA r5 for macOS. Includes Standalone, AU, VST3 and Metal GPU support.
This development build is ad-hoc signed, without Apple Developer ID or notarization.
Close your DAW before installation. Use the ARA region-extension workflow.
Models download separately inside the app. Transcription starts with Transcribe.
EOF
pkgbuild --root "$payload" --identifier com.arifgasimli.neuralnote.ara --version 2.0.0.5 --install-location / --ownership recommended "$out/NeuralNote_ARA_Installer_Mac_${label}_r5.pkg"
ditto -c -k --sequesterRsrc --keepParent "$payload" "$out/NeuralNote-ARA-Mac-${label}-r5.zip"
(cd "$out" && shasum -a 256 *.pkg *.zip > "SHA256SUMS_Mac_${label}.txt")
