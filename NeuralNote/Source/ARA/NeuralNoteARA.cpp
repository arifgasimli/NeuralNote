#include "NeuralNoteARA.h"

#if JucePlugin_Enable_ARA
#include <map>
#include <limits>

namespace nn::ara
{
namespace
{
juce::String clipKey(const juce::ARAPlaybackRegion& region)
{
    // Source-time geometry identifies the analysed excerpt even when it moves on the timeline.
    return juce::String(region.getStartInAudioModificationTime(), 9) + ":"
        + juce::String(region.getDurationInAudioModificationTime(), 9);
}

class Modification final : public juce::ARAAudioModification
{
public:
    Modification(juce::ARAAudioSource* source, ARA::ARAAudioModificationHostRef ref,
                 const juce::ARAAudioModification* clone)
        : ARAAudioModification(source, ref, clone)
    {
        if (const auto* other = dynamic_cast<const Modification*>(clone))
            restore(other->save());
    }

    ~Modification() override { invalidate(); }

    std::shared_ptr<ClipState> stateFor(const juce::String& key)
    {
        auto& state = states[key];
        if (!state) {
            state = std::make_shared<ClipState>();
            state->onChanged = [this] {
                // Private persisted state changed; rendered source audio is still identical.
                notifyContentChanged(juce::ARAContentUpdateScopes::nothingIsAffected(), true);
            };
        }
        return state;
    }

    juce::ValueTree save() const
    {
        juce::ValueTree tree("clips");
        for (const auto& [key, state] : states) {
            const juce::ScopedLock lock(state->lock);
            juce::ValueTree clip("clip");
            clip.setProperty("range", key, nullptr);
            if (state->transcription.isValid())
                clip.appendChild(state->transcription.createCopy(), nullptr);
            tree.appendChild(clip, nullptr);
        }
        return tree;
    }

    void restore(const juce::ValueTree& tree)
    {
        for (const auto& clip : tree) {
            auto state = stateFor(clip["range"].toString());
            const juce::ScopedLock lock(state->lock);
            state->transcription = clip.getChild(0).createCopy();
        }
    }

    void invalidate()
    {
        for (const auto& [key, state] : states) {
            juce::ignoreUnused(key);
            state->onChanged = nullptr;
        }
        states.clear();
    }

private:
    std::map<juce::String, std::shared_ptr<ClipState>> states;
};

class ReaderThread final : public juce::TimeSliceThread
{
public:
    ReaderThread() : TimeSliceThread("NeuralNote ARA audio") { startThread(); }
    ~ReaderThread() override { stopThread(-1); }
};

// ARA replaces the input stream with playback-region assignments. Read ahead off the audio
// thread, and take a non-blocking lock while the host is editing its model graph.
class Renderer final : public juce::ARAPlaybackRenderer, private juce::ARAAudioSource::Listener
{
public:
    Renderer(ARA::PlugIn::DocumentController* dc, juce::ReadWriteLock& modelLock)
        : ARAPlaybackRenderer(dc), editLock(modelLock) {}

    ~Renderer() override { releaseResources(); }

    void prepareToPlay(double rate, int blockSize, int,
                       juce::AudioProcessor::ProcessingPrecision, AlwaysNonRealtime nonRealtime) override
    {
        releaseResources();
        outputRate = rate;
        maximumBlockSize = blockSize;
        alwaysNonRealtime = nonRealtime;
        for (auto* region : getPlaybackRegions()) {
            auto* source = region->getAudioModification()->getAudioSource();
            if (readers.count(source) != 0)
                continue;
            source->addListener(this);
            rebuild(source);
        }
    }

    void releaseResources() override
    {
        for (const auto& [source, entry] : readers) {
            juce::ignoreUnused(entry);
            source->removeListener(this);
        }
        readers.clear();
    }

    bool processBlock(juce::AudioBuffer<float>& buffer, juce::AudioProcessor::Realtime realtime,
                      const juce::AudioPlayHead::PositionInfo& position) noexcept override
    {
        buffer.clear();
        const juce::ScopedTryReadLock lock(editLock);
        if (!lock.isLocked() || !position.getIsPlaying())
            return true;

        const auto blockStart = position.getTimeInSamples().orFallback(
            static_cast<juce::int64>(std::llround(position.getTimeInSeconds().orFallback(0.0) * outputRate)));
        const juce::Range<juce::int64> block(blockStart, blockStart + buffer.getNumSamples());
        bool success = true;
        for (auto* region : getPlaybackRegions()) {
            auto* source = region->getAudioModification()->getAudioSource();
            const auto it = readers.find(source);
            if (it == readers.end() || !source->isSampleAccessEnabled())
                continue;
            const auto range = block.getIntersectionWith(region->getSampleRange(
                outputRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no));
            if (range.isEmpty())
                continue;

            auto& entry = it->second;
            const double ratio = source->getSampleRate() / outputRate;
            const double sourceStart = (region->getStartInAudioModificationTime()
                + static_cast<double>(range.getStart()) / outputRate - region->getStartInPlaybackTime())
                * source->getSampleRate();
            const auto first = static_cast<juce::int64>(std::floor(sourceStart));
            const double fraction = sourceStart - static_cast<double>(first);
            const int count = static_cast<int>(std::ceil(fraction + static_cast<double>(range.getLength()) * ratio)) + 1;
            if (count > entry.scratch.getNumSamples()) {
                success = false;
                continue;
            }
            if (entry.buffered)
                entry.buffered->setReadTimeout(realtime == juce::AudioProcessor::Realtime::yes ? 0 : -1);
            if (!entry.reader->read(&entry.scratch, 0, count, first, true, true)) {
                success = false;
                continue;
            }
            const int destStart = static_cast<int>(range.getStart() - blockStart);
            for (int c = 0; c < buffer.getNumChannels(); ++c) {
                auto* dest = buffer.getWritePointer(c, destStart);
                // Fold multichannel sources to mono, or map their channels to stereo output.
                for (int i = 0; i < static_cast<int>(range.getLength()); ++i) {
                    const double index = fraction + i * ratio;
                    const int j = static_cast<int>(index);
                    const float alpha = static_cast<float>(index - j);
                    float value = 0;
                    const int channels = entry.scratch.getNumChannels();
                    for (int s = 0; s < channels; ++s) {
                        if (buffer.getNumChannels() != 1 && s != juce::jmin(c, channels - 1))
                            continue;
                        const auto* samples = entry.scratch.getReadPointer(s);
                        value += samples[j] + alpha * (samples[j + 1] - samples[j]);
                    }
                    dest[i] += buffer.getNumChannels() == 1 ? value / static_cast<float>(channels) : value;
                }
            }
        }
        return success;
    }

    using ARAPlaybackRenderer::processBlock;

private:
    struct Entry {
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::BufferingAudioReader* buffered = nullptr;
        juce::AudioBuffer<float> scratch;
    };

    void rebuild(juce::ARAAudioSource* source)
    {
        const juce::ScopedWriteLock lock(editLock);
        Entry entry;
        auto reader = std::make_unique<juce::ARAAudioSourceReader>(source);
        const auto ratio = source->getSampleRate() / outputRate;
        entry.scratch.setSize(source->getChannelCount(), static_cast<int>(std::ceil(maximumBlockSize * ratio)) + 3);
        if (alwaysNonRealtime == AlwaysNonRealtime::yes) {
            entry.reader = std::move(reader);
        } else {
            auto buffered = std::make_unique<juce::BufferingAudioReader>(reader.release(), *thread,
                juce::jmax(32768, static_cast<int>(source->getSampleRate() * 2)));
            entry.buffered = buffered.get();
            entry.reader = std::move(buffered);
        }
        readers.insert_or_assign(source, std::move(entry));
    }

    void didUpdateAudioSourceProperties(juce::ARAAudioSource* source) override { rebuild(source); }
    void doUpdateAudioSourceContent(juce::ARAAudioSource* source, juce::ARAContentUpdateScopes scopes) override
    {
        if (scopes.affectSamples())
            rebuild(source);
    }
    void didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* source, bool enabled) override
    {
        if (enabled)
            rebuild(source);
    }
    void willDestroyAudioSource(juce::ARAAudioSource* source) override
    {
        const juce::ScopedWriteLock lock(editLock);
        source->removeListener(this);
        readers.erase(source);
    }
    juce::ReadWriteLock& editLock;
    juce::SharedResourcePointer<ReaderThread> thread;
    std::map<juce::ARAAudioSource*, Entry> readers;
    double outputRate = 44100;
    int maximumBlockSize = 512;
    AlwaysNonRealtime alwaysNonRealtime = AlwaysNonRealtime::no;
};

class Controller final : public juce::ARADocumentControllerSpecialisation
{
public:
    using ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;

private:
    void willBeginEditing(juce::ARADocument*) override { editLock.enterWrite(); }
    void didEndEditing(juce::ARADocument*) override { editLock.exitWrite(); }

    void willUpdateAudioSourceProperties(juce::ARAAudioSource* source,
                                         juce::ARAAudioSource::PropertiesPtr properties) override
    {
        if (source->getSampleCount() != properties->sampleCount
            || source->getSampleRate() != properties->sampleRate
            || source->getChannelCount() != properties->channelCount)
            for (auto* modification : source->getAudioModifications<Modification>())
                modification->invalidate();
    }

    void doUpdateAudioSourceContent(juce::ARAAudioSource* source, juce::ARAContentUpdateScopes scopes) override
    {
        if (scopes.affectSamples())
            for (auto* modification : source->getAudioModifications<Modification>())
                modification->invalidate();
    }

    juce::ARAAudioModification* doCreateAudioModification(juce::ARAAudioSource* source,
        ARA::ARAAudioModificationHostRef ref, const juce::ARAAudioModification* clone) override
    {
        return new Modification(source, ref, clone);
    }

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override
    {
        return new Renderer(getDocumentController(), editLock);
    }

    bool doStoreObjectsToStream(juce::ARAOutputStream& stream, const ARA::PlugIn::StoreObjectsFilter* filter) noexcept override
    {
        const auto& modifications = filter->getAudioModificationsToStore<Modification>();
        if (!stream.writeInt(1) || !stream.writeInt(static_cast<int>(modifications.size())))
            return false;
        for (auto* modification : modifications) {
            if (!stream.writeString(modification->getPersistentID()))
                return false;
            juce::MemoryOutputStream encoded;
            modification->save().writeToStream(encoded);
            if (!stream.write(encoded.getData(), encoded.getDataSize()))
                return false;
        }
        return true;
    }

    bool doRestoreObjectsFromStream(juce::ARAInputStream& stream, const ARA::PlugIn::RestoreObjectsFilter* filter) noexcept override
    {
        if (stream.readInt() != 1)
            return false;
        const int count = stream.readInt();
        if (count < 0 || count > 100000)
            return false;
        for (int i = 0; i < count; ++i) {
            const auto id = stream.readString();
            const auto tree = juce::ValueTree::readFromStream(stream);
            if (!tree.hasType("clips") || stream.failed())
                return false;
            if (auto* modification = filter->getAudioModificationToRestoreStateWithID<Modification>(id.toRawUTF8()))
                modification->restore(tree);
        }
        return !stream.failed();
    }

    juce::ReadWriteLock editLock;
};

std::vector<juce::ARAPlaybackRegion*> regionsFor(juce::AudioProcessorARAExtension& processor, bool includeSelection)
{
    std::vector<juce::ARAPlaybackRegion*> regions;
    const auto add = [&regions](juce::ARAPlaybackRegion* region) {
        if (std::find(regions.begin(), regions.end(), region) == regions.end())
            regions.push_back(region);
    };
    if (includeSelection)
        if (auto* view = processor.getEditorView())
            for (auto* region : view->getViewSelection().getEffectivePlaybackRegions<juce::ARAPlaybackRegion>())
                add(region);
    if (auto* renderer = processor.getPlaybackRenderer())
        for (auto* region : renderer->getPlaybackRegions())
            add(region);
    if (auto* renderer = processor.getEditorRenderer()) {
        for (auto* region : renderer->getPlaybackRegions())
            add(region);
        for (auto* sequence : renderer->getRegionSequences())
            for (auto* region : sequence->getPlaybackRegions())
                add(region);
    }

    return regions;
}

juce::String regionId(const juce::ARAPlaybackRegion& region)
{
    return juce::String(region.getAudioModification()->getPersistentID()) + "/" + clipKey(region);
}
}

std::vector<Clip> getClips(juce::AudioProcessorARAExtension& processor, bool includeSelection)
{
    std::vector<Clip> clips;
    for (auto* region : regionsFor(processor, includeSelection)) {
        auto* source = region->getAudioModification()->getAudioSource();
        const char* regionName = region->getName();
        const char* sourceName = source->getName();
        const auto name = regionName != nullptr ? regionName : sourceName != nullptr ? sourceName : "Host clip";
        clips.push_back({regionId(*region), juce::String(name) + " ("
            + juce::String(region->getStartInPlaybackTime(), 2) + " s)"});
    }
    return clips;
}

bool getClipStart(juce::AudioProcessorARAExtension& processor, const juce::String& id, double& start, bool includeSelection)
{
    for (auto* region : regionsFor(processor, includeSelection))
        if (regionId(*region) == id) {
            start = region->getStartInPlaybackTime();
            return true;
        }
    return false;
}

ARA::PlugIn::HostPlaybackController* getPlaybackController(juce::AudioProcessorARAExtension& processor)
{
    return processor.isBoundToARA() ? processor.getDocumentController()->getHostPlaybackController() : nullptr;
}

juce::Result readClip(juce::AudioProcessorARAExtension& processor, const juce::String& id, ImportedClip& out, bool includeSelection)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    for (auto* region : regionsFor(processor, includeSelection)) {
        if (regionId(*region) != id)
            continue;
        auto* source = region->getAudioModification()->getAudioSource();
        if (!source->isSampleAccessEnabled())
            return juce::Result::fail("The host has temporarily disabled audio access. Try again when it has finished loading.");
        if (region->isTimestretchEnabled())
            return juce::Result::fail("Render or bounce time-stretched clips before importing them into NeuralNote.");
        const double rate = source->getSampleRate();
        const auto start = static_cast<juce::int64>(std::llround(region->getStartInAudioModificationTime() * rate));
        const double samples = std::round(region->getDurationInPlaybackTime() * rate);
        if (!std::isfinite(samples) || samples < 1 || samples > std::numeric_limits<int>::max()
            || rate <= 0 || source->getChannelCount() < 1)
            return juce::Result::fail("This clip has an unsupported duration or audio format.");

        juce::ARAAudioSourceReader reader(source);
        out.audio.setSize(source->getChannelCount(), static_cast<int>(samples));
        // All model access stays on the document thread. The ML job later reads our own snapshot.
        if (!reader.isValid() || !reader.read(&out.audio, 0, out.audio.getNumSamples(), start, true, true))
            return juce::Result::fail("The host could not supply this clip's audio. Try again once the clip is online.");
        out.sampleRate = rate;
        out.playbackStart = region->getStartInPlaybackTime();
        const char* sourceName = source->getName();
        out.name = sourceName != nullptr ? sourceName : "Host clip";
        out.state = region->getAudioModification<Modification>()->stateFor(clipKey(*region));
        return juce::Result::ok();
    }
    return juce::Result::fail("This clip was removed or changed. Open the host-clip menu again.");
}
}

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<nn::ara::Controller>();
}
#endif
