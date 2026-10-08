//
// Created by Damien Ronssin on 20.08.26.
//

#include "NnEditorConstrainer.h"

#include "NnLook.h"

void NnEditorConstrainer::configure(juce::Component* inEditor)
{
    mEditor = inEditor;

    setFixedAspectRatio(0.0);

    setMinimumWidth(juce::roundToIntAccurate((double) nn::metrics::editorWidth * nn::metrics::minEditorScale));
    setMinimumHeight(240);

    _applyMaximumSizeForCurrentDisplay();
}

double NnEditorConstrainer::maxScaleForCurrentDisplay() const
{
    const juce::Displays::Display* display = nullptr;

    if (mEditor != nullptr) {
        const auto centre = mEditor->getScreenBounds().getCentre();
        display = juce::Desktop::getInstance().getDisplays().getDisplayForPoint(centre.toFloat());
    }

    if (display == nullptr) {
        display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    }

    if (display == nullptr) {
        return nn::metrics::maxEditorScale;
    }

    const auto usable_area = display->userBounds.toDouble();

    const double fit_width = usable_area.getWidth() * MAX_DISPLAY_PERCENT_WIDTH / (double) nn::metrics::editorWidth;
    const double fit_height =
        usable_area.getHeight() * MAX_DISPLAY_PERCENT_HEIGHT / (double) nn::metrics::editorHeight;

    return juce::jmin(fit_width, fit_height, nn::metrics::maxEditorScale);
}

double NnEditorConstrainer::clampScale(double inScale) const
{
    // Not jlimit: on a display too small for even the minimum, the minimum still wins -- a window
    // clipped at the bottom beats one whose text is unreadable.
    return juce::jmax(nn::metrics::minEditorScale, juce::jmin(inScale, maxScaleForCurrentDisplay()));
}

void NnEditorConstrainer::checkBounds(juce::Rectangle<int>& ioBounds,
                                      const juce::Rectangle<int>& inPreviousBounds,
                                      const juce::Rectangle<int>& inLimits,
                                      bool inIsStretchingTop,
                                      bool inIsStretchingLeft,
                                      bool inIsStretchingBottom,
                                      bool inIsStretchingRight)
{
    _applyMaximumSizeForCurrentDisplay();

    juce::ComponentBoundsConstrainer::checkBounds(ioBounds,
                                                  inPreviousBounds,
                                                  inLimits,
                                                  inIsStretchingTop,
                                                  inIsStretchingLeft,
                                                  inIsStretchingBottom,
                                                  inIsStretchingRight);
}

void NnEditorConstrainer::resizeEnd()
{
    if (onResizeEnd != nullptr) {
        onResizeEnd();
    }
}

void NnEditorConstrainer::_applyMaximumSizeForCurrentDisplay()
{
    // Embedded ARA editors must accept the full host pane, including ultrawide layouts.
    setMaximumWidth(32768);
    setMaximumHeight(32768);
}
