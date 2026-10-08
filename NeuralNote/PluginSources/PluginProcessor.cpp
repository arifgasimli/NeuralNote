#include "PluginProcessor.h"
#include "PluginEditor.h"

NeuralNoteAudioProcessor::NeuralNoteAudioProcessor()
    : mAPVTS(*this, nullptr, NnId::ParametersId, ParameterHelpers::createParameterLayout())
{
    // Enable logging or not
#if 0
    mLogger.reset(FileLogger::createDefaultAppLogger("/tmp/NeuralNote", "log.txt", "YO! \n"));
    Logger::setCurrentLogger(mLogger.get());
#endif

    for (size_t i = 0; i < mParams.size(); i++) {
        auto pid = static_cast<ParameterHelpers::ParamIdEnum>(i);
        mParams[i] = mAPVTS.getParameter(ParameterHelpers::getIdStr(pid));
    }

    mSourceAudioManager = std::make_unique<SourceAudioManager>(this);
    mPlayer = std::make_unique<Player>(this);
    mTranscriptionManager = std::make_unique<TranscriptionManager>(this);

    // After mPlayer: it pushes every fader it holds into that player's synth.
    mInstrumentMixer = std::make_unique<InstrumentMixer>(this);
    startTimer(100);
}

NeuralNoteAudioProcessor::~NeuralNoteAudioProcessor()
{
    stopTimer();
    Logger::setCurrentLogger(nullptr);
}

void NeuralNoteAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    mSourceAudioManager->prepareToPlay(sampleRate, samplesPerBlock);
    mPlayer->prepareToPlay(sampleRate, samplesPerBlock);
#if JucePlugin_Enable_ARA
    prepareToPlayForARA(sampleRate, samplesPerBlock, getTotalNumOutputChannels(), getProcessingPrecision());
#endif
}

void NeuralNoteAudioProcessor::releaseResources()
{
#if JucePlugin_Enable_ARA
    releaseResourcesForARA();
#endif
}

void NeuralNoteAudioProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer& midiMessages)
{
    // Host playhead access belongs to the audio callback; ValueTree/UI writes belong to the timer.
    double bpm = 0;
    if (const auto* playhead = getPlayHead()) {
        if (const auto position = playhead->getPosition(); position.hasValue())
            bpm = position->getBpm().orFallback(0.0);
    }
    mHostTempo = std::isfinite(bpm) && bpm > 0 ? bpm : 0;
#if JucePlugin_Enable_ARA
    processBlockForARA(buffer, isRealtime(), getPlayHead());
#endif
    mSourceAudioManager->processBlock(buffer);

    auto is_mute = mParams[ParameterHelpers::MuteId]->getValue() > 0.5f;

    if (is_mute) {
        buffer.clear();
    }

    mPlayer->processBlock(buffer, midiMessages);
}

AudioProcessorEditor* NeuralNoteAudioProcessor::createEditor()
{
    return new NeuralNoteEditor(*this);
}

void NeuralNoteAudioProcessor::getStateInformation(MemoryBlock& destData)
{
    auto full_state_tree = ValueTree(NnId::FullStateId);

    full_state_tree.setProperty(NnId::NeuralNoteVersionId, ProjectInfo::versionString, nullptr);

    // PARAMETERS
    auto apvts = mAPVTS.copyState();
    jassert(apvts.getType() == NnId::ParametersId);

    full_state_tree.appendChild(apvts, nullptr);

    // NEURAL NOTE STATE
    // Update value tree with current state
    mPlayer->saveStateToValueTree();

    full_state_tree.appendChild(mValueTree, nullptr);

    if (auto transcription_tree = mTranscriptionManager->createStateTree(); transcription_tree.isValid()) {
        full_state_tree.appendChild(transcription_tree, nullptr);
    }

    std::unique_ptr<XmlElement> xml(full_state_tree.createXml());

    if (xml != nullptr) {
        copyXmlToBinary(*xml, destData);
    }
}

void NeuralNoteAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    // Create an XmlElement from the binary data
    std::unique_ptr<XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    if (xmlState != nullptr) {
        // Convert XmlElement to ValueTree
        ValueTree full_state_tree = ValueTree::fromXml(*xmlState);

        if (full_state_tree.isValid() && full_state_tree.hasType(NnId::FullStateId)) {
            // Extract the parameters ValueTree
            auto parameter_tree = full_state_tree.getChildWithName(NnId::ParametersId);

            ParameterHelpers::updateParametersFromState(parameter_tree, mParams);

        } else {
            jassertfalse;
        }

        if (full_state_tree.isValid() && full_state_tree.hasType(NnId::FullStateId)) {
            auto new_value_tree = full_state_tree.getChildWithName(NnId::NeuralNoteStateId);
            _updateValueTree(new_value_tree);

            // After the value tree, which is what loads the audio the notes belong to.
            mTranscriptionManager->restoreFromStateTree(full_state_tree.getChildWithName(NnId::TranscriptionId));
        } else {
            jassertfalse;
        }
    }
}

void NeuralNoteAudioProcessor::clear()
{
#if JucePlugin_Enable_ARA
    mARAClipActive = false;
    mARAClipId.clear();
    mARAClipState.reset();
#endif
    mPlayer->reset();
    mSourceAudioManager->clear();
    mTranscriptionManager->clear();

    mState.store(EmptyAudioAndMidiRegions);

    // Every clear path has to reset the view too, not just the clear button: a cancelled or failed
    // transcription otherwise leaves the audio region sized and scrolled for audio that is gone.
    if (auto* main_view = getNeuralNoteMainView()) {
        main_view->clear();
    }
}

void NeuralNoteAudioProcessor::clearTranscription()
{
    mPlayer->reset();
    mTranscriptionManager->clear();

    // Falls back to Empty rather than asserting: a run that failed before any audio was acquired
    // has nothing to go back to.
    mState.store(mSourceAudioManager->getNumSamplesDownAcquired() > 0 ? AudioLoaded : EmptyAudioAndMidiRegions);
#if JucePlugin_Enable_ARA
    saveARATranscription();
#endif

    // Re-sizes the audio region from the current sample count, which is unchanged here -- so this
    // is the same call as clear()'s and it leaves the waveform where it is.
    if (auto* main_view = getNeuralNoteMainView()) {
        main_view->clear();
    }
}

SourceAudioManager* NeuralNoteAudioProcessor::getSourceAudioManager() const
{
    return mSourceAudioManager.get();
}

Player* NeuralNoteAudioProcessor::getPlayer() const
{
    return mPlayer.get();
}

TranscriptionManager* NeuralNoteAudioProcessor::getTranscriptionManager() const
{
    return mTranscriptionManager.get();
}

InstrumentMixer* NeuralNoteAudioProcessor::getInstrumentMixer() const
{
    return mInstrumentMixer.get();
}

float NeuralNoteAudioProcessor::getParameterValue(ParameterHelpers::ParamIdEnum inParamId) const
{
    return ParameterHelpers::getUnmappedParamValue(mParams[inParamId]);
}

NeuralNoteMainView* NeuralNoteAudioProcessor::getNeuralNoteMainView() const
{
    auto* editor = dynamic_cast<NeuralNoteEditor*>(getActiveEditor());

    if (editor != nullptr) {
        return editor->getMainView();
    }

    return nullptr;
}

AudioProcessorValueTreeState& NeuralNoteAudioProcessor::getAPVTS()
{
    return mAPVTS;
}

ValueTree& NeuralNoteAudioProcessor::getValueTree()
{
    return mValueTree;
}

void NeuralNoteAudioProcessor::addListenerToStateValueTree(ValueTree::Listener* inListener)
{
    mValueTree.addListener(inListener);
}

void NeuralNoteAudioProcessor::removeListenerFromStateValueTree(ValueTree::Listener* inListener)
{
    mValueTree.removeListener(inListener);
}

ValueTree NeuralNoteAudioProcessor::_createDefaultValueTree()
{
    ValueTree default_value_tree(NnId::NeuralNoteStateId);

    for (const auto& [id, default_value]: NnId::OrderedStatePropertiesWithDefault) {
        default_value_tree.setProperty(id, default_value, nullptr);
    }

    return default_value_tree;
}

void NeuralNoteAudioProcessor::_updateValueTree(const ValueTree& inNewState)
{
    jassert(inNewState.getType() == NnId::NeuralNoteStateId);
    jassert(mValueTree.getNumProperties() == static_cast<int>(NnId::OrderedStatePropertiesWithDefault.size()));

    if (inNewState.isValid()) {
        // Set all properties from inNewState to mValueTree, ignoring extra ones if any.
        // If less, missing properties will be left as is.
        for (const auto& [prop_id, default_val]: NnId::OrderedStatePropertiesWithDefault) {
            if (inNewState.hasProperty(prop_id)) {
                mValueTree.setProperty(prop_id, inNewState.getProperty(prop_id), nullptr);
            }
        }

        // Children are not covered by the loop above, and the instrument mixer is one. Replaced
        // wholesale rather than merged: the saved set of instruments is the authority, and a stale
        // node left behind would keep muting an instrument the reloaded session no longer has.
        if (const auto saved_mixer = inNewState.getChildWithName(NnId::InstrumentMixerId); saved_mixer.isValid()) {
            mValueTree.removeChild(mValueTree.getChildWithName(NnId::InstrumentMixerId), nullptr);
            mValueTree.appendChild(saved_mixer.createCopy(), nullptr);
        }
    } else {
        jassertfalse;
    }
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NeuralNoteAudioProcessor();
}

#if JucePlugin_Enable_ARA
juce::Result NeuralNoteAudioProcessor::importARAClip(const juce::String& id)
{
    if (getState() == Recording || getState() == Processing)
        return juce::Result::fail("Stop recording or wait for transcription to finish before importing a host clip.");

    nn::ara::ImportedClip clip;
    if (const auto result = nn::ara::readClip(*this, id, clip, getActiveEditor() != nullptr); result.failed())
        return result;

    // The source/player buffers must not be replaced while an audio callback is reading them.
    const bool wasSuspended = isSuspended();
    suspendProcessing(true);
    const juce::ScopeGuard resume([this, wasSuspended] { suspendProcessing(wasSuspended); });
    const juce::ScopedLock audioCallbackLock(getCallbackLock());
    if (const auto result = mSourceAudioManager->onHostAudio(clip.audio, clip.sampleRate, clip.name); result.failed())
        return result;

    mARAClipState = std::move(clip.state);
    mARAClipId = id;
    mARAClipStart = clip.playbackStart;
    mARAClipActive = true;
    mARAAutoImportDone = true;
    juce::ValueTree saved;
    {
        const juce::ScopedLock lock(mARAClipState->lock);
        saved = mARAClipState->transcription.createCopy();
    }
    mTranscriptionManager->restoreFromStateTree(saved);
    return juce::Result::ok();
}

void NeuralNoteAudioProcessor::saveARATranscription()
{
    if (mARAClipState) {
        const auto tree = mTranscriptionManager->createStateTree();
        mARAClipState->setTranscription(tree);
    }
}
#endif

void NeuralNoteAudioProcessor::syncHostTempo()
{
    const double bpm = mHostTempo.load();
    const bool linked = bpm > 0;
    if (linked && std::abs(static_cast<double>(mValueTree.getProperty(NnId::ExportTempoId, 120.0)) - bpm) > 0.000001)
        mValueTree.setProperty(NnId::ExportTempoId, bpm, nullptr);

}

void NeuralNoteAudioProcessor::timerCallback()
{
    syncHostTempo();
#if JucePlugin_Enable_ARA
    syncARAHostState();
#endif
}

#if JucePlugin_Enable_ARA
void NeuralNoteAudioProcessor::syncARAHostState()
{
    if (!isBoundToARA())
        return;
    if (!mARAAutoImportDone && getState() != Recording && getState() != Processing
        && Time::getMillisecondCounterHiRes() >= mARANextImportAttempt) {
        const auto clips = nn::ara::getClips(*this, getActiveEditor() != nullptr);
        if (!clips.empty()) {
            mARANextImportAttempt = Time::getMillisecondCounterHiRes() + 1500;
            importARAClip(clips.front().id);
        }
    }
    if (!mARAClipId.isEmpty()) {
        double start = 0;
        mARAClipActive = nn::ara::getClipStart(*this, mARAClipId, start, getActiveEditor() != nullptr);
        mARAClipStart = start;
    }
}
#endif

bool NeuralNoteAudioProcessor::requestHostPlayback(bool playing)
{
#if JucePlugin_Enable_ARA
    if (isARATransportLinked()) {
        if (auto* controller = nn::ara::getPlaybackController(*this)) {
            if (playing) controller->requestStartPlayback();
            else controller->requestStopPlayback();
        }
        return true;
    }
#endif
    juce::ignoreUnused(playing);
    return false;
}

bool NeuralNoteAudioProcessor::requestHostPosition(double localSeconds)
{
#if JucePlugin_Enable_ARA
    if (isARATransportLinked()) {
        if (auto* controller = nn::ara::getPlaybackController(*this))
            controller->requestSetPlaybackPosition(getARAClipStart() + juce::jlimit(0.0,
                mSourceAudioManager->getAudioSampleDuration(), localSeconds));
        return true;
    }
#endif
    juce::ignoreUnused(localSeconds);
    return false;
}
