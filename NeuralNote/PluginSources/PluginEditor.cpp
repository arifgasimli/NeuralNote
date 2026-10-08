#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "NnGlobalSettings.h"
#include "NnLook.h"

NeuralNoteEditor::NeuralNoteEditor(NeuralNoteAudioProcessor& p)
    : AudioProcessorEditor(&p)
#if JucePlugin_Enable_ARA
    , AudioProcessorEditorARAExtension(&p)
#endif
{
    mMainView = std::make_unique<NeuralNoteMainView>(p);

    addAndMakeVisible(*mMainView);
#if JucePlugin_Enable_ARA
    p.syncARAHostState();
#endif

    mConstrainer.configure(this);
    mConstrainer.onResizeEnd = [this] { _persistScale(); };
    setConstrainer(&mConstrainer);
    setResizable(true, true);

    // Once per editor, so a window opened after the file changed honours what it now holds.
    NnGlobalSettings::reload();

    mPersistedScale = NnGlobalSettings::getEditorScale();
    _setScale(mPersistedScale);
}

NeuralNoteEditor::~NeuralNoteEditor()
{
    // The standalone's native-frame drags never reach mConstrainer.onResizeEnd, so closing the
    // window is where the size they ended at gets stored. First, before the teardown below.
    _persistScale();

    // mConstrainer dies with this class, before ~Component destroys the peer and the corner
    // resizer -- both of which hold a raw pointer to it and can still be resized during teardown.
    setResizable(false, false);
    setConstrainer(nullptr);
}

void NeuralNoteEditor::paint(juce::Graphics& g)
{
    g.fillAll(nn::colours::bgRoot);
}

void NeuralNoteEditor::resized()
{
    // A zero extent would make the transform singular, and there is nothing to lay out at it.
    if (getLocalBounds().isEmpty()) {
        return;
    }

    // Keep controls at the selected UI scale while the timeline fills both host dimensions.
    // Narrow windows may scale down to keep the full toolbar accessible.
    const double scale = juce::jmin(mScale, static_cast<double>(getWidth()) / nn::metrics::editorWidth);
    mMainView->setTransform(juce::AffineTransform::scale(static_cast<float>(scale)));
    mMainView->setBounds(0, 0, juce::roundToInt(std::ceil(getWidth() / scale)),
                        juce::roundToInt(std::ceil(getHeight() / scale)));
}

void NeuralNoteEditor::parentHierarchyChanged()
{
    // The display is only known once the editor is attached, which it is not while the constructor
    // runs. Detachment comes through here too, and clamping a window on its way out only risks
    // resizing it during teardown.
    if (!isOnDesktop() && getParentComponent() == nullptr) {
        return;
    }

    resized();
}

void NeuralNoteEditor::applyScale(double inScale)
{
    // mScale is what was applied, not what was asked for: a preset the display cannot hold
    // arrives clamped, and the clamped one is what gets stored.
    _setScale(inScale);
    _persistScale();
}

void NeuralNoteEditor::_setScale(double inScale)
{
    const double scale = mConstrainer.clampScale(inScale);
    mScale = scale;

    setSize(juce::roundToIntAccurate((double) nn::metrics::editorWidth * scale),
            juce::roundToIntAccurate((double) nn::metrics::editorHeight * scale));
}

void NeuralNoteEditor::_persistScale()
{
    if (std::abs(mScale - mPersistedScale) < 0.0005) {
        return;
    }

    mPersistedScale = mScale;
    NnGlobalSettings::setEditorScale(mScale);
}
