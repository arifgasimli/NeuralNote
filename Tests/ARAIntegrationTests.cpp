#include "NeuralNoteARA.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ComputeDevices.h"
#include <ARA_Library/Dispatch/ARAHostDispatch.h>
#include <cmath>
#include <cstdio>
#include <limits>

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();

namespace
{
int failures = 0;
void check(bool ok, const char* message)
{
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

class Host final : public ARA::Host::AudioAccessControllerInterface,
                   public ARA::Host::ArchivingControllerInterface,
                   public ARA::Host::PlaybackControllerInterface
{
public:
    double value = 0.25;
    juce::MemoryBlock archive;
    bool requestedPlaying = false;
    double requestedPosition = 0;
    void requestStartPlayback() noexcept override { requestedPlaying = true; }
    void requestStopPlayback() noexcept override { requestedPlaying = false; }
    void requestSetPlaybackPosition(double position) noexcept override { requestedPosition = position; }
    void requestSetCycleRange(double, double) noexcept override {}
    void requestEnableCycle(bool) noexcept override {}
    ARA::ARAAudioReaderHostRef createAudioReaderForSource(ARA::ARAAudioSourceHostRef, bool use64) noexcept override
    {
        check(!use64, "read 32-bit host samples");
        return reinterpret_cast<ARA::ARAAudioReaderHostRef>(this);
    }
    bool readAudioSamples(ARA::ARAAudioReaderHostRef, ARA::ARASamplePosition start,
                          ARA::ARASampleCount count, void* const buffers[]) noexcept override
    {
        for (int c = 0; c < 2; ++c)
            for (ARA::ARASampleCount i = 0; i < count; ++i)
                static_cast<float*>(buffers[c])[i] = static_cast<float>(value + c * 0.25 + (start + i) * 0.000001);
        return true;
    }
    void destroyAudioReader(ARA::ARAAudioReaderHostRef) noexcept override {}
    ARA::ARASize getArchiveSize(ARA::ARAArchiveReaderHostRef) noexcept override { return archive.getSize(); }
    bool readBytesFromArchive(ARA::ARAArchiveReaderHostRef, ARA::ARASize pos, ARA::ARASize size,
                              ARA::ARAByte* dest) noexcept override
    {
        if (pos + size > archive.getSize())
            return false;
        archive.copyTo(dest, static_cast<int>(pos), size);
        return true;
    }
    bool writeBytesToArchive(ARA::ARAArchiveWriterHostRef, ARA::ARASize pos, ARA::ARASize size,
                             const ARA::ARAByte* src) noexcept override
    {
        archive.ensureSize(pos + size);
        archive.copyFrom(src, static_cast<int>(pos), size);
        return true;
    }
    void notifyDocumentArchivingProgress(float) noexcept override {}
    void notifyDocumentUnarchivingProgress(float) noexcept override {}
    ARA::ARAPersistentID getDocumentArchiveID(ARA::ARAArchiveReaderHostRef) noexcept override
    {
        return "com.draudio.neuralnote.ara.archive.1";
    }
};

class TestPlayhead final : public juce::AudioPlayHead
{
public:
    PositionInfo position;
    juce::Optional<PositionInfo> getPosition() const override { return position; }
};
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    const auto* factory = createARAFactory();
    ARA::ARAInterfaceConfiguration config {};
    config.structSize = sizeof(config);
    config.desiredApiGeneration = ARA::kARAAPIGeneration_2_0_Final;
    factory->initializeARAWithConfiguration(&config);

    Host host;
    ARA::Host::DocumentControllerHostInstance hostInstance(&host, &host, nullptr, nullptr, &host);
    ARA::ARADocumentProperties document {};
    document.structSize = sizeof(document);
    document.name = "Test document";
    const auto* instance = factory->createDocumentControllerWithDocument(&hostInstance, &document);
    ARA::Host::DocumentController dc(instance);
    dc.beginEditing();

    ARA::ARAMusicalContextProperties context {};
    context.structSize = sizeof(context);
    context.name = "Test context";
    auto contextRef = dc.createMusicalContext(reinterpret_cast<ARA::ARAMusicalContextHostRef>(&host), &context);
    ARA::ARARegionSequenceProperties sequence {};
    sequence.structSize = sizeof(sequence);
    sequence.name = "Track";
    sequence.musicalContextRef = contextRef;
    auto sequenceRef = dc.createRegionSequence(reinterpret_cast<ARA::ARARegionSequenceHostRef>(&host), &sequence);
    ARA::ARAAudioSourceProperties source {};
    source.structSize = sizeof(source);
    source.name = "Stereo source";
    source.persistentID = "source-1";
    source.sampleCount = 48000;
    source.sampleRate = 48000;
    source.channelCount = 2;
    auto sourceRef = dc.createAudioSource(reinterpret_cast<ARA::ARAAudioSourceHostRef>(&host), &source);
    dc.enableAudioSourceSamplesAccess(sourceRef, true);
    ARA::ARAAudioModificationProperties modification {};
    modification.structSize = sizeof(modification);
    modification.persistentID = "mod-1";
    auto modificationRef = dc.createAudioModification(sourceRef,
        reinterpret_cast<ARA::ARAAudioModificationHostRef>(&host), &modification);
    ARA::ARAPlaybackRegionProperties region {};
    region.structSize = sizeof(region);
    region.name = "Trimmed clip";
    region.regionSequenceRef = sequenceRef;
    region.startInModificationTime = 0.25;
    region.durationInModificationTime = 0.5;
    region.startInPlaybackTime = 2.0;
    region.durationInPlaybackTime = 0.5;
    auto regionRef = dc.createPlaybackRegion(modificationRef,
        reinterpret_cast<ARA::ARAPlaybackRegionHostRef>(&host), &region);
    dc.endEditing();

    {
        NeuralNoteAudioProcessor extension;
        constexpr auto roles = ARA::kARAPlaybackRendererRole;
        extension.bindToARA(instance->documentControllerRef, roles, roles);
        auto* renderer = extension.getPlaybackRenderer();
        renderer->addPlaybackRegion(regionRef);
        TestPlayhead playhead;
        extension.setPlayHead(&playhead);
        extension.setNonRealtime(true);
        extension.prepareToPlay(48000, 128);
        extension.syncARAHostState();
        check(extension.getState() == AudioLoaded, "automatically import assigned ARA clip without an editor");
        check(extension.isARATransportLinked(), "link transport after auto import");
        juce::AudioBuffer<float> hostBuffer(2, 128);
        juce::MidiBuffer hostMidi;
        playhead.position.setIsPlaying(true);
        playhead.position.setTimeInSeconds(2.1);
        playhead.position.setTimeInSamples(100800);
        extension.processBlock(hostBuffer, hostMidi);
        check(extension.getPlayer()->isPlaying(), "follow host Play");
        check(std::abs(extension.getPlayer()->getPlayheadPositionSeconds() - 0.1) < 0.000001,
              "map host position to clip-relative playhead");
        playhead.position.setIsPlaying(false);
        playhead.position.setTimeInSeconds(2.35);
        playhead.position.setTimeInSamples(112800);
        extension.processBlock(hostBuffer, hostMidi);
        check(!extension.getPlayer()->isPlaying(), "follow host Stop");
        check(std::abs(extension.getPlayer()->getPlayheadPositionSeconds() - 0.35) < 0.000001,
              "follow seeks while host is stopped");
        playhead.position.setBpm(137.25);
        extension.processBlock(hostBuffer, hostMidi);
        extension.syncHostTempo();
        check(extension.isHostTempoLinked(), "link plugin tempo to host BPM");
        check(std::abs(static_cast<double>(extension.getValueTree()[NnId::ExportTempoId]) - 137.25) < 0.000001,
              "follow fractional project tempo while stopped");
        playhead.position.setBpm(96.5);
        extension.processBlock(hostBuffer, hostMidi);
        extension.syncHostTempo();
        check(std::abs(static_cast<double>(extension.getValueTree()[NnId::ExportTempoId]) - 96.5) < 0.000001,
              "follow project tempo changes");
        playhead.position.setBpm(std::numeric_limits<double>::quiet_NaN());
        extension.processBlock(hostBuffer, hostMidi);
        extension.syncHostTempo();
        check(!extension.isHostTempoLinked(), "reject invalid host tempo");
        extension.getValueTree().setProperty(NnId::ExportTempoId, 112.0, nullptr);
        playhead.position.setBpm(juce::nullopt);
        extension.processBlock(hostBuffer, hostMidi);
        extension.syncHostTempo();
        check(static_cast<double>(extension.getValueTree()[NnId::ExportTempoId]) == 112.0,
              "preserve manual tempo when host BPM is unavailable");
        extension.getPlayer()->setPlayingState(true);
        check(host.requestedPlaying, "plugin Play requests host playback");
        extension.getPlayer()->setPlayheadPositionSeconds(0.2);
        check(std::abs(host.requestedPosition - 2.2) < 0.000001, "plugin seek requests absolute host position");
        extension.getPlayer()->setPlayingState(false);
        check(!host.requestedPlaying, "plugin Stop requests host stop");
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor(extension.createEditor());
            editor->setSize(2668, 813);
            auto* mainView = static_cast<NeuralNoteEditor*>(editor.get())->getMainView();
            auto mapped = juce::Point<float>(static_cast<float>(mainView->getWidth()), static_cast<float>(mainView->getHeight())).transformedBy(mainView->getTransform());
            check(std::abs(mapped.x - 2668) < 2 && std::abs(mapped.y - 813) < 2,
                  "editor content fills Studio One's wide pane");
            editor->setSize(1700, 420);
            mapped = juce::Point<float>(static_cast<float>(mainView->getWidth()), static_cast<float>(mainView->getHeight())).transformedBy(mainView->getTransform());
            check(std::abs(mapped.x - 1700) < 2 && std::abs(mapped.y - 420) < 2,
                  "editor height resizes independently of width");
        }
        if (juce::SystemStats::getEnvironmentVariable("NEURALNOTE_INFERENCE_SMOKE", "") == "1") {
            MuscriptorEngine engine;
            engine.reset();
            const auto& samples = extension.getSourceAudioManager()->getDownsampledSourceAudioForTranscription();
            ComputeDeviceChoice device;
            const auto requestedDevice = juce::SystemStats::getEnvironmentVariable("NEURALNOTE_INFERENCE_DEVICE", "");
            const auto& devices = ComputeDevices::get();
            bool found = requestedDevice.isEmpty();
            for (std::size_t i = 0; i < devices.size(); ++i) {
                std::printf("Compute device: %s (%s)\n", devices[i].name.c_str(), devices[i].backend.c_str());
                if (juce::String(devices[i].name) == requestedDevice) {
                    device = ComputeDevices::choiceFor(devices, i);
                    found = true;
                }
            }
            check(found, "explicit inference device is enumerated");
            const auto model = modelSizeFromString(juce::SystemStats::getEnvironmentVariable(
                "NEURALNOTE_INFERENCE_MODEL", "small").toStdString(), ModelSize::Small);
            const auto result = found ? engine.transcribeToMIDI(model, device, samples.getReadPointer(0),
                extension.getSourceAudioManager()->getNumSamplesDownAcquired(), {}) : MuscriptorEngine::Outcome::Failed;
            check(result == MuscriptorEngine::Outcome::Success, "real model inference on autoimported ARA clip");
            std::printf("Inference smoke result: %d\n", static_cast<int>(result));
        }
        extension.releaseResources();
        const auto clips = nn::ara::getClips(extension);
        check(clips.size() == 1, "enumerate assigned clip");
        nn::ara::ImportedClip imported;
        check(nn::ara::readClip(extension, clips.front().id, imported).wasOk(), "import host clip");
        check(imported.audio.getNumSamples() == 24000, "respect clip duration");
        check(std::abs(imported.audio.getSample(0, 0) - 0.262f) < 0.00001f, "respect source trim offset");
        check(std::abs(imported.audio.getSample(1, 0) - 0.512f) < 0.00001f, "preserve stereo source");
        juce::ValueTree testNotes("testNotes");
        testNotes.setProperty("pitch", 64, nullptr);
        imported.state->setTranscription(testNotes);
        const auto writerRef = reinterpret_cast<ARA::ARAArchiveWriterHostRef>(&host);
        const auto readerRef = reinterpret_cast<ARA::ARAArchiveReaderHostRef>(&host);
        check(dc.storeObjectsToArchive(writerRef, nullptr), "archive clip notes");
        imported.state->transcription = {};
        dc.beginEditing();
        check(dc.restoreObjectsFromArchive(readerRef, nullptr), "restore clip notes");
        dc.endEditing();
        check(static_cast<int>(imported.state->transcription["pitch"]) == 64, "recover archived transcription");

        dc.beginEditing();
        modification.persistentID = "mod-clone";
        const auto cloneRef = dc.cloneAudioModification(modificationRef,
            reinterpret_cast<ARA::ARAAudioModificationHostRef>(&modification), &modification);
        auto cloneRegion = region;
        cloneRegion.startInPlaybackTime = 3.0;
        const auto cloneRegionRef = dc.createPlaybackRegion(cloneRef,
            reinterpret_cast<ARA::ARAPlaybackRegionHostRef>(&region), &cloneRegion);
        dc.endEditing();
        renderer->addPlaybackRegion(cloneRegionRef);
        const auto clonedClips = nn::ara::getClips(extension);
        nn::ara::ImportedClip cloned;
        check(clonedClips.size() == 2, "enumerate cloned modification");
        check(nn::ara::readClip(extension, clonedClips.back().id, cloned).wasOk(), "import cloned modification");
        check(static_cast<int>(cloned.state->transcription["pitch"]) == 64, "clone transcription with modification");
        ARA::ARAStoreObjectsFilter storeFilter {};
        storeFilter.structSize = sizeof(storeFilter);
        storeFilter.audioModificationRefsCount = 1;
        storeFilter.audioModificationRefs = &modificationRef;
        host.archive.reset();
        check(dc.storeObjectsToArchive(writerRef, &storeFilter), "save a single modification with a partial archive filter");
        juce::MemoryInputStream archiveHeader(host.archive, false);
        check(archiveHeader.readInt() == 1 && archiveHeader.readInt() == 1, "partial archive excludes other modifications");
        renderer->removePlaybackRegion(cloneRegionRef);
        dc.beginEditing();
        dc.destroyPlaybackRegion(cloneRegionRef);
        dc.destroyAudioModification(cloneRef);
        dc.endEditing();

        renderer->prepareToPlay(44100, 128, 1, juce::AudioProcessor::singlePrecision,
                                juce::ARARenderer::AlwaysNonRealtime::yes);
        juce::AudioBuffer<float> output(1, 128);
        juce::AudioPlayHead::PositionInfo position;
        position.setIsPlaying(true);
        position.setTimeInSamples(88200);
        check(renderer->processBlock(output, juce::AudioProcessor::Realtime::no, position), "render at a different host sample rate");
        check(std::abs(output.getSample(0, 0) - 0.387f) < 0.00001f, "fold stereo to mono on playback");
        position.setIsPlaying(false);
        renderer->processBlock(output, juce::AudioProcessor::Realtime::no, position);
        check(output.getMagnitude(0, 128) == 0, "silence while host transport is stopped");

        dc.beginEditing();
        host.value = 0.5;
        dc.updateAudioSourceContent(sourceRef, nullptr, ARA::ContentUpdateScopes::samplesAreAffected());
        dc.endEditing();
        position.setIsPlaying(true);
        check(renderer->processBlock(output, juce::AudioProcessor::Realtime::no, position), "rebuild reader after host source edit");
        check(std::abs(output.getSample(0, 0) - 0.637f) < 0.00001f, "render updated host samples");
        check(nn::ara::readClip(extension, clips.front().id, imported).wasOk(), "reimport changed source");
        check(!imported.state->transcription.isValid(), "discard stale transcription after host source edit");

        dc.enableAudioSourceSamplesAccess(sourceRef, false);
        check(nn::ara::readClip(extension, clips.front().id, imported).failed(), "reject disabled host audio access");
        dc.enableAudioSourceSamplesAccess(sourceRef, true);
        renderer->releaseResources();

        renderer->prepareToPlay(48000, 128, 2, juce::AudioProcessor::singlePrecision,
                                juce::ARARenderer::AlwaysNonRealtime::no);
        juce::AudioBuffer<float> bufferedOutput(2, 128);
        position.setTimeInSamples(96000);
        check(renderer->processBlock(bufferedOutput, juce::AudioProcessor::Realtime::no, position), "read through background audio cache");
        check(std::abs(bufferedOutput.getSample(0, 0) - 0.512f) < 0.00001f, "buffered reader returns current samples");
        dc.beginEditing();
        host.value = 0.75;
        dc.updateAudioSourceContent(sourceRef, nullptr, ARA::ContentUpdateScopes::samplesAreAffected());
        dc.endEditing();
        check(renderer->processBlock(bufferedOutput, juce::AudioProcessor::Realtime::no, position), "refresh background audio cache after source edit");
        check(std::abs(bufferedOutput.getSample(0, 0) - 0.762f) < 0.00001f, "background cache does not return stale samples");
        renderer->releaseResources();
        renderer->removePlaybackRegion(regionRef);
        dc.beginEditing();
        dc.destroyPlaybackRegion(regionRef);
        dc.endEditing();
        check(nn::ara::readClip(extension, clips.front().id, imported).failed(), "reject stale menu selection after region deletion");
    }
    dc.beginEditing();
    dc.destroyAudioModification(modificationRef);
    dc.destroyAudioSource(sourceRef);
    dc.destroyRegionSequence(sequenceRef);
    dc.destroyMusicalContext(contextRef);
    dc.endEditing();
    dc.destroyDocumentController();
    factory->uninitializeARA();
    std::printf("ARA integration: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
