#pragma once

#include "NeuralNoteMainView.h"
#include "NnEditorConstrainer.h"
#include "PluginProcessor.h"

/** Maps the selected control scale onto a main view that fills the resizable host pane. */
class NeuralNoteEditor : public juce::AudioProcessorEditor
#if JucePlugin_Enable_ARA
    , public juce::AudioProcessorEditorARAExtension
#endif
{
public:
    explicit NeuralNoteEditor(NeuralNoteAudioProcessor&);

    ~NeuralNoteEditor() override;

    void paint(juce::Graphics&) override;

    void resized() override;

    void parentHierarchyChanged() override;

    NeuralNoteMainView* getMainView() const { return mMainView.get(); }

    /** Sizes the window from inScale, clamped to the display, and persists what it settled on. */
    void applyScale(double inScale);

    /** The factor actually in force, which a scale too large for the display will not match. */
    double getAppliedScale() const { return mScale; }

private:
    /** Sizes the window without persisting, for the paths that are not the user asking. */
    void _setScale(double inScale);

    /** Stores mScale globally, unless it is already what was stored. */
    void _persistScale();

    std::unique_ptr<NeuralNoteMainView> mMainView;

    NnEditorConstrainer mConstrainer;

    double mScale = 1.0;

    // What global.settings holds, so that opening and closing a window untouched writes nothing.
    double mPersistedScale = 1.0;
};
