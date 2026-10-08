#pragma once

#include <JuceHeader.h>

#if JucePlugin_Enable_ARA
namespace nn::ara
{
// Shared with the processor so deleting a host region cannot leave a dangling model pointer.
struct ClipState {
    juce::CriticalSection lock;
    juce::ValueTree transcription;
    std::function<void()> onChanged;

    // Message thread only, like all ARA model changes. The lock also protects archive reads.
    void setTranscription(const juce::ValueTree& tree)
    {
        {
            const juce::ScopedLock guard(lock);
            if (transcription.isEquivalentTo(tree))
                return;
            transcription = tree;
        }
        if (onChanged)
            onChanged();
    }
};

struct Clip {
    // Resolve this ID again when a menu action runs; never retain host model pointers in a menu.
    juce::String id;
    juce::String name;
};

struct ImportedClip {
    juce::AudioBuffer<float> audio;
    double sampleRate = 0;
    double playbackStart = 0;
    juce::String name;
    std::shared_ptr<ClipState> state;
};

std::vector<Clip> getClips(juce::AudioProcessorARAExtension& processor, bool includeSelection = true);
bool getClipStart(juce::AudioProcessorARAExtension& processor, const juce::String& id, double& start, bool includeSelection);
ARA::PlugIn::HostPlaybackController* getPlaybackController(juce::AudioProcessorARAExtension& processor);
juce::Result readClip(juce::AudioProcessorARAExtension& processor, const juce::String& id, ImportedClip& out, bool includeSelection = true);
}
#endif
