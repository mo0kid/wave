#include "PluginEditor.h"

#include "PanelWiring.h"

#include <BinaryData.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
constexpr auto designWidth = 2338.0f;
constexpr auto designHeight = 1042.0f;
constexpr auto panelOnlyHeight = 619.0f;
constexpr auto panelHorizontalOffset = 66.0f;
constexpr auto minimumEditorScale = 0.5f;
constexpr auto maximumEditorScale = 2.0f;
constexpr auto defaultEditorScale = 1.0f;
constexpr auto keyboardLeft = 446.0f;
constexpr auto keyboardTop = 764.0f;
constexpr auto keyboardWidth = 1671.0f;
constexpr auto keyboardWhiteKeyWidth = keyboardWidth / 36.0f;
constexpr auto keyboardWhiteKeyHeight = 278.0f;
constexpr auto keyboardBlackKeyWidth = keyboardWhiteKeyWidth * 0.62f;
constexpr auto keyboardBlackKeyHeight = keyboardWhiteKeyHeight * 0.62f;
constexpr auto keyboardLowestMidiNote = 36;
constexpr auto keyboardHighestMidiNote = 96;
constexpr auto recentMidiFeedbackMilliseconds = 180.0;
const std::array<juce::Rectangle<float>, 3> performanceWheelSlots {
    juce::Rectangle<float> { 101.0f, 816.0f, 19.0f, 127.0f },
    juce::Rectangle<float> { 160.0f, 816.0f, 19.0f, 127.0f },
    juce::Rectangle<float> { 219.0f, 816.0f, 19.0f, 127.0f }
};
constexpr std::array<int, 13> performanceKeys {
    'a', 'w', 's', 'e', 'd', 'f', 't', 'g', 'y', 'h', 'u', 'j', 'k'
};
constexpr std::array<int, 7> whiteKeySemitones { 0, 2, 4, 5, 7, 9, 11 };
constexpr std::array<int, 8> instrumentSwitches { 22, 25, 26, 27, 79, 28, 29, 30 };
constexpr auto loadFirmwareMenuItem = 0x4707;
constexpr auto mountDiskMenuItem = 0x4701;
constexpr auto saveDiskMenuItem = 0x4702;
constexpr auto ejectDiskMenuItem = 0x4703;
constexpr auto createDiskFromSetMenuItem = 0x4704;
constexpr auto createBlankDiskMenuItem = 0x4705;
constexpr auto saveDiskAsMenuItem = 0x4706;
constexpr auto zoomInMenuItem = 0x4708;
constexpr auto zoomOutMenuItem = 0x4709;
constexpr auto resetZoomMenuItem = 0x470a;
constexpr auto toggleKeyboardMenuItem = 0x470b;
constexpr auto loadPanelSkinMenuItem = 0x470c;
constexpr auto defaultPanelSkinMenuItem = 0x470d;
constexpr auto tabbedLayoutMenuItem = 0x470e;
constexpr auto voiceAllocationFixMenuItem = 0x470f;

// The tabbed layout reuses the original artwork and wiring unchanged: each tab
// is a list of full-SVG rectangles translated into a smaller view. The screen
// block keeps the same view position on every tab.
struct LayoutBlock
{
    juce::Rectangle<float> source;
    juce::Point<float> destination;

    [[nodiscard]] juce::Rectangle<float> destinationBounds() const noexcept
    {
        return source.withPosition(destination);
    }
};

constexpr auto tabStripHeight = 40.0f;
constexpr auto tabSectionWidth = 834.0f;
constexpr auto tabbedViewWidth = tabSectionWidth + 612.0f;
constexpr auto tabbedViewHeight = tabStripHeight + panelOnlyHeight;
const std::array<juce::String, 3> tabNames { "OSCILLATOR", "FILTER", "CONTROL" };
const auto tabHeaderColour = juce::Colour(0xffd1ae88);
const auto panelDividerColour = juce::Colour(0xff24232d); // screen frame lines

juce::Rectangle<float> tabBounds(int tab) noexcept
{
    return { 54.0f + static_cast<float>(tab) * 168.0f, 8.0f, 160.0f, 26.0f };
}

const std::vector<LayoutBlock>& layoutBlocksFor(bool tabbed, int tab)
{
    static const std::vector<LayoutBlock> classic {
        { { 0.0f, 0.0f, designWidth, designHeight }, {} }
    };
    const LayoutBlock screen { { 900.0f, 0.0f, 612.0f, panelOnlyHeight },
                               { tabSectionWidth, tabStripHeight } };
    // Transport row sits in the free space above the screen; Rewind's left
    // edge lines up with the Mute button's.
    const LayoutBlock transport { { 950.0f, 640.0f, 460.0f, 66.0f },
                                  { tabSectionWidth + 37.0f, 48.0f } };
    // System Volume follows the transport row, level with its buttons.
    const LayoutBlock systemVolume { { 330.0f, 880.0f, 76.0f, 78.0f },
                                     { tabSectionWidth + 480.0f, 40.0f } };
    const juce::Point<float> controlOrigin {
        (tabSectionWidth - 380.0f) * 0.5f,
        tabStripHeight + (panelOnlyHeight - 300.0f) * 0.5f
    };
    static const std::array<std::vector<LayoutBlock>, 3> tabs {
        std::vector<LayoutBlock> {
            { { panelHorizontalOffset, 0.0f, tabSectionWidth, panelOnlyHeight },
              { 0.0f, tabStripHeight } },
            screen, transport, systemVolume
        },
        std::vector<LayoutBlock> {
            // Right-aligned so its content keeps the Oscillator tab's margin
            // to the screen.
            { { 1512.0f, 0.0f, 760.0f, panelOnlyHeight }, { 69.0f, tabStripHeight } },
            screen, transport, systemVolume
        },
        std::vector<LayoutBlock> {
            // Wheels, Button 1/2, Glide and octave switches.
            // Stops short of x = 446, where the runtime keybed begins.
            { { panelHorizontalOffset, 682.0f, 372.0f, 300.0f }, controlOrigin },
            // Blank panel over System Volume's original spot, which has moved.
            { { 318.0f, 794.0f, 80.0f, 80.0f },
              controlOrigin + juce::Point<float> { 328.0f - panelHorizontalOffset,
                                                   879.0f - 682.0f } },
            screen, transport, systemVolume
        }
    };
    return tabbed ? tabs[static_cast<size_t>(juce::jlimit(0, 2, tab))] : classic;
}

bool hitCircle(juce::Point<float> point, float x, float y) noexcept
{
    // The artwork is 30 px across; retain a small, non-overlapping interaction
    // margin so antialiased edge taps still operate the visible switch.
    return point.getDistanceFrom({ x, y }) <= 18.0f;
}

bool hitRectangle(juce::Point<float> point, float x, float y,
                  float width, float height) noexcept
{
    return juce::Rectangle<float> { x, y, width, height }.contains(point);
}

bool isBlackMidiNote(int midiNote) noexcept
{
    const auto pitchClass = (midiNote - keyboardLowestMidiNote) % 12;
    return pitchClass == 1 || pitchClass == 3 || pitchClass == 6
           || pitchClass == 8 || pitchClass == 10;
}

int whiteKeyIndexForMidiNote(int midiNote) noexcept
{
    const auto semitones = midiNote - keyboardLowestMidiNote;
    const auto octave = semitones / 12;
    const auto pitchClass = semitones % 12;
    auto whiteKeyInOctave = 0;
    switch (pitchClass)
    {
        case 0: case 1: whiteKeyInOctave = 0; break;
        case 2: case 3: whiteKeyInOctave = 1; break;
        case 4: whiteKeyInOctave = 2; break;
        case 5: case 6: whiteKeyInOctave = 3; break;
        case 7: case 8: whiteKeyInOctave = 4; break;
        case 9: case 10: whiteKeyInOctave = 5; break;
        case 11: whiteKeyInOctave = 6; break;
        default: break;
    }
    return octave * 7 + whiteKeyInOctave;
}

juce::Rectangle<float> midiKeyBounds(int midiNote) noexcept
{
    if (midiNote < keyboardLowestMidiNote || midiNote > keyboardHighestMidiNote)
        return {};

    const auto whiteKeyIndex = whiteKeyIndexForMidiNote(midiNote);
    if (!isBlackMidiNote(midiNote))
        return { keyboardLeft + static_cast<float>(whiteKeyIndex) * keyboardWhiteKeyWidth,
                 keyboardTop, keyboardWhiteKeyWidth, keyboardWhiteKeyHeight };

    // Distribute the visible gaps evenly within each group of two or three
    // black keys instead of centring every key on a white-key boundary.
    const auto semitones = midiNote - keyboardLowestMidiNote;
    const auto octave = semitones / 12;
    const auto pitchClass = semitones % 12;
    constexpr auto blackWidthInWhiteKeys = keyboardBlackKeyWidth / keyboardWhiteKeyWidth;
    constexpr auto halfBlackWidth = blackWidthInWhiteKeys * 0.5f;
    constexpr auto twoKeyGap = (3.0f - 2.0f * blackWidthInWhiteKeys) / 3.0f;
    constexpr auto threeKeyGap = (4.0f - 3.0f * blackWidthInWhiteKeys) / 4.0f;

    auto centreInOctave = 0.0f;
    switch (pitchClass)
    {
        case 1: centreInOctave = twoKeyGap + halfBlackWidth; break;
        case 3: centreInOctave = 2.0f * twoKeyGap
                                  + 1.5f * blackWidthInWhiteKeys; break;
        case 6: centreInOctave = 3.0f + threeKeyGap + halfBlackWidth; break;
        case 8: centreInOctave = 3.0f + 2.0f * threeKeyGap
                                  + 1.5f * blackWidthInWhiteKeys; break;
        case 10: centreInOctave = 3.0f + 3.0f * threeKeyGap
                                   + 2.5f * blackWidthInWhiteKeys; break;
        default: break;
    }

    const auto centreX = keyboardLeft
                         + (static_cast<float>(octave * 7) + centreInOctave)
                               * keyboardWhiteKeyWidth;
    return { centreX - keyboardBlackKeyWidth * 0.5f, keyboardTop,
             keyboardBlackKeyWidth, keyboardBlackKeyHeight };
}

int midiNoteAt(juce::Point<float> point) noexcept
{
    const auto keyboardBounds = juce::Rectangle<float> {
        keyboardLeft, keyboardTop, keyboardWhiteKeyWidth * 36.0f,
        keyboardWhiteKeyHeight
    };
    if (!keyboardBounds.contains(point))
        return -1;

    if (point.y < keyboardTop + keyboardBlackKeyHeight)
        for (auto midiNote = keyboardLowestMidiNote;
             midiNote <= keyboardHighestMidiNote; ++midiNote)
            if (isBlackMidiNote(midiNote) && midiKeyBounds(midiNote).contains(point))
                return midiNote;

    const auto whiteKeyIndex = juce::jlimit(
        0, 35, static_cast<int>((point.x - keyboardLeft) / keyboardWhiteKeyWidth));
    const auto octave = whiteKeyIndex / 7;
    const auto keyInOctave = whiteKeyIndex % 7;
    return juce::jmin(keyboardHighestMidiNote,
                      keyboardLowestMidiNote + octave * 12
                          + whiteKeySemitones[static_cast<size_t>(keyInOctave)]);
}

}

class WaveEmulationAudioProcessorEditor::WaveLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider(juce::Graphics&, int, int, int, int, float, float, float,
                          juce::Slider&) override
    {
    }

    void drawLinearSlider(juce::Graphics&, int, int, int, int, float, float, float,
                          juce::Slider::SliderStyle, juce::Slider&) override
    {
    }

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&,
                              bool, bool) override
    {
    }

    void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override
    {
    }
};

class WaveEmulationAudioProcessorEditor::ParameterKnob final
    : public juce::Component,
      public juce::SettableTooltipClient
{
public:
    ParameterKnob(juce::AudioProcessorValueTreeState& state, const juce::String& parameterId,
                  const juce::String& displayName, bool useVerticalFader,
                  bool useEndlessRelative)
        : parameterState(state), endlessRelative(useEndlessRelative)
    {
        slider.setSliderStyle(useVerticalFader ? juce::Slider::LinearVertical
                                               : juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setMouseDragSensitivity(180);
        // These Wave controls are physical endless encoders. Let this component
        // handle their relative movement so a drag never jumps to an absolute
        // position or stops the artwork at either end of a parameter's range.
        slider.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(slider);
        setBinding(parameterId, displayName);
    }

    void resized() override
    {
        slider.setBounds(getLocalBounds());
    }

    [[nodiscard]] float normalisedValue() noexcept
    {
        return static_cast<float>(slider.valueToProportionOfLength(slider.getValue()));
    }

    [[nodiscard]] float artworkAngle(float boundedRotationExtent) noexcept
    {
        if (endlessRelative)
            return std::remainder(endlessArtworkAngle,
                                  juce::MathConstants<float>::twoPi);

        return juce::jmap(physicalArtworkPosition,
                          -boundedRotationExtent, boundedRotationExtent);
    }

    void setBinding(const juce::String& parameterId, const juce::String& displayName)
    {
        attachment.reset();
        boundParameterId = parameterId;
        attachedParameter = parameterState.getParameter(parameterId);
        if (attachedParameter != nullptr)
            slider.setDoubleClickReturnValue(true, attachedParameter->convertFrom0to1(
                                                       attachedParameter->getDefaultValue()));
        setTooltip(displayName + "\nDrag up/down to adjust. Hold Shift for fine adjustment.");
        if (endlessRelative)
            attachment = std::make_unique<Attachment>(parameterState, parameterId, slider);
        else if (!physicalPositionInitialised && attachedParameter != nullptr)
        {
            physicalArtworkPosition = attachedParameter->getValue();
            physicalPositionInitialised = true;
        }
    }

    void setEncoderTurnCallback(std::function<void(int)> callback)
    {
        encoderTurnCallback = std::move(callback);
    }

    void setEncoderPixelsPerStep(float pixels) noexcept
    {
        encoderPixelsPerStep = juce::jmax(1.0f, pixels);
    }

    void setPhysicalPotCallback(std::function<void(const juce::String&, float)> callback)
    {
        physicalPotCallback = std::move(callback);
    }

    void synchronisePhysicalPosition(float normalised) noexcept
    {
        if (endlessRelative || boundedDragging)
            return;
        physicalArtworkPosition = juce::jlimit(0.0f, 1.0f, normalised);
        physicalPositionInitialised = true;
    }

    [[nodiscard]] const juce::String& parameterId() const noexcept
    {
        return boundParameterId;
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (attachedParameter == nullptr)
            return;

        if (!endlessRelative)
        {
            boundedDragging = true;
            lastDragOffsetY = 0;
            attachedParameter->beginChangeGesture();
            sendPhysicalPotValue();
            return;
        }

        dragging = true;
        lastDragOffsetY = 0;
        encoderStepAccumulator = 0.0f;
        attachedParameter->beginChangeGesture();
        event.source.enableUnboundedMouseMovement(true, false);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (boundedDragging)
        {
            const auto dragOffsetY = event.getOffsetFromDragStart().y;
            const auto pixels = static_cast<float>(lastDragOffsetY - dragOffsetY);
            lastDragOffsetY = dragOffsetY;
            const auto fineScale = event.mods.isShiftDown() ? 0.25f : 1.0f;
            physicalArtworkPosition = juce::jlimit(
                0.0f, 1.0f,
                physicalArtworkPosition + pixels * fineScale / dragPixelsForFullRange);
            sendPhysicalPotValue();
            repaintArtwork();
            return;
        }

        if (!dragging || attachedParameter == nullptr)
            return;

        const auto dragOffsetY = event.getOffsetFromDragStart().y;
        const auto pixels = static_cast<float>(lastDragOffsetY - dragOffsetY);
        lastDragOffsetY = dragOffsetY;
        if (pixels == 0.0f)
            return;

        const auto fineScale = event.mods.isShiftDown() ? 0.25f : 1.0f;
        const auto normalisedDelta = pixels * fineScale / dragPixelsForFullRange;
        if (encoderPixelsPerStep > 0.0f)
        {
            encoderStepAccumulator += pixels * fineScale / encoderPixelsPerStep;
            const auto steps = encoderStepAccumulator >= 0.0f
                                   ? static_cast<int>(std::floor(encoderStepAccumulator))
                                   : static_cast<int>(std::ceil(encoderStepAccumulator));
            if (steps != 0)
            {
                encoderStepAccumulator -= static_cast<float>(steps);
                const auto parameterIntervals = juce::jmax(
                    1, attachedParameter->getNumSteps() - 1);
                attachedParameter->setValueNotifyingHost(
                    juce::jlimit(0.0f, 1.0f,
                                 attachedParameter->getValue()
                                     + static_cast<float>(steps)
                                           / static_cast<float>(parameterIntervals)));
                if (encoderTurnCallback)
                    encoderTurnCallback(steps);
            }
        }
        else
        {
            attachedParameter->setValueNotifyingHost(
                juce::jlimit(0.0f, 1.0f,
                             attachedParameter->getValue() + normalisedDelta));
            if (encoderTurnCallback)
            {
                encoderStepAccumulator += normalisedDelta * 127.0f;
                const auto steps = encoderStepAccumulator >= 0.0f
                                       ? static_cast<int>(std::floor(
                                             encoderStepAccumulator))
                                       : static_cast<int>(std::ceil(
                                             encoderStepAccumulator));
                if (steps != 0)
                {
                    encoderStepAccumulator -= static_cast<float>(steps);
                    encoderTurnCallback(steps);
                }
            }
        }
        endlessArtworkAngle += normalisedDelta
                               * juce::MathConstants<float>::twoPi;
        repaintArtwork();
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (boundedDragging)
        {
            if (attachedParameter != nullptr)
                attachedParameter->endChangeGesture();
            boundedDragging = false;
            return;
        }

        if (!dragging)
            return;

        event.source.enableUnboundedMouseMovement(false);
        if (attachedParameter != nullptr)
            attachedParameter->endChangeGesture();
        dragging = false;
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (!endlessRelative || attachedParameter == nullptr)
            return;

        attachedParameter->beginChangeGesture();
        attachedParameter->setValueNotifyingHost(attachedParameter->getDefaultValue());
        attachedParameter->endChangeGesture();
    }

private:
    void sendPhysicalPotValue()
    {
        if (physicalPotCallback != nullptr && boundParameterId.isNotEmpty())
            physicalPotCallback(boundParameterId, physicalArtworkPosition);
    }

    void repaintArtwork()
    {
        if (auto* parent = getParentComponent())
            parent->repaint(getBounds());
    }

    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    static constexpr auto dragPixelsForFullRange = 180.0f;
    juce::AudioProcessorValueTreeState& parameterState;
    juce::Slider slider;
    std::unique_ptr<Attachment> attachment;
    std::function<void(int)> encoderTurnCallback;
    std::function<void(const juce::String&, float)> physicalPotCallback;
    juce::RangedAudioParameter* attachedParameter = nullptr;
    juce::String boundParameterId;
    int lastDragOffsetY = 0;
    float endlessArtworkAngle = 0.0f;
    float physicalArtworkPosition = 0.5f;
    float encoderStepAccumulator = 0.0f;
    float encoderPixelsPerStep = 0.0f;
    bool endlessRelative = false;
    bool physicalPositionInitialised = false;
    bool dragging = false;
    bool boundedDragging = false;
};

class WaveEmulationAudioProcessorEditor::ArtworkButton final
    : public juce::Button
{
public:
    explicit ArtworkButton(juce::Drawable* suppliedArtwork)
        : juce::Button("Edit"), artwork(suppliedArtwork)
    {
        setTooltip("Edit");
    }

    void paintButton(juce::Graphics& graphics, bool, bool) override
    {
        if (artwork != nullptr)
            artwork->drawWithin(graphics, getLocalBounds().toFloat(),
                                juce::RectanglePlacement::stretchToFit, 1.0f);
    }

    bool hitTest(int x, int y) override
    {
        // Exact path from button_edit.svg. Keeping the hit area on the pill
        // prevents its transparent square corners stealing nearby controls.
        // The editor is resizable, so transform the SVG's 65 x 65 viewBox to
        // this component's current local bounds before testing the pointer.
        juce::Path path;
        path.addRoundedRectangle(0.0f, 43.1335f, 61.0f, 30.0f, 15.0f);
        path.applyTransform(juce::AffineTransform::rotation(
            -juce::MathConstants<float>::pi * 0.25f, 0.0f, 43.1335f));
        path.applyTransform(juce::AffineTransform::scale(
            static_cast<float>(getWidth()) / 65.0f,
            static_cast<float>(getHeight()) / 65.0f));
        return path.contains(static_cast<float>(x), static_cast<float>(y));
    }

private:
    juce::Drawable* artwork = nullptr;
};

class WaveEmulationAudioProcessorEditor::SystemMenuButton final : public juce::Button
{
public:
    SystemMenuButton() : juce::Button("Disk image menu")
    {
        setTooltip("System / Disk images");
        setMouseClickGrabsKeyboardFocus(false);
    }

    void paintButton(juce::Graphics& graphics, bool highlighted, bool pressed) override
    {
        const auto side = juce::jmin(static_cast<float>(getWidth()),
                                     static_cast<float>(getHeight())) * 0.68f;
        const auto icon = juce::Rectangle<float> { side, side }
                              .withCentre(getLocalBounds().toFloat().getCentre());
        auto colour = iconColour;
        if (pressed)
            colour = colour.darker(0.3f);
        else if (highlighted)
            colour = colour.brighter(0.4f);

        juce::Path body;
        body.startNewSubPath(icon.getX(), icon.getY());
        body.lineTo(icon.getRight() - side * 0.24f, icon.getY());
        body.lineTo(icon.getRight(), icon.getY() + side * 0.24f);
        body.lineTo(icon.getRight(), icon.getBottom());
        body.lineTo(icon.getX(), icon.getBottom());
        body.closeSubPath();

        const auto stroke = juce::jmax(1.0f, side * 0.085f);
        graphics.setColour(colour);
        graphics.strokePath(body, juce::PathStrokeType(stroke,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
        graphics.drawRect(
            icon.withTrimmedLeft(side * 0.22f).withTrimmedRight(side * 0.28f)
                .withTrimmedTop(side * 0.12f).withHeight(side * 0.30f),
            stroke);
        graphics.drawRoundedRectangle(
            icon.withTrimmedLeft(side * 0.18f).withTrimmedRight(side * 0.18f)
                .withTrimmedTop(side * 0.57f).withTrimmedBottom(side * 0.12f),
            side * 0.06f, stroke);
    }

    juce::Colour iconColour { 0xffdce4ef };
};

WaveEmulationAudioProcessorEditor::WaveEmulationAudioProcessorEditor(
    WaveEmulationAudioProcessor& owner)
    : AudioProcessorEditor(owner), ownerProcessor(owner), lookAndFeel(std::make_unique<WaveLookAndFeel>())
{
    setLookAndFeel(lookAndFeel.get());
    panelAnalogValues.fill(0.0f);
    keyboardNotes.fill(false);
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
    systemMenuButton = std::make_unique<SystemMenuButton>();
    systemMenuButton->onClick = [this] { showSystemMenu(); };
    addAndMakeVisible(*systemMenuButton);
    setResizable(true, true);
    tabbedLayout = ownerProcessor.getRememberedTabbedLayout();

    const auto svg = juce::String::fromUTF8(WaveAssets::WaldorfWaveUI_NOLOGO_svg,
                                            WaveAssets::WaldorfWaveUI_NOLOGO_svgSize);
    if (const auto xml = juce::XmlDocument::parse(svg))
    {
        panelArtwork = juce::Drawable::createFromSVG(*xml);
        initialisePanelRegions(*xml);
    }
    const auto rememberedSkin = ownerProcessor.getRememberedPanelSkin();
    if (rememberedSkin != juce::File{})
        loadPanelSkin(rememberedSkin, false); // Missing/invalid artwork falls back to the bundled panel.
    const auto sliderSvg = juce::String::fromUTF8(WaveAssets::Slider_svg,
                                                  WaveAssets::Slider_svgSize);
    if (const auto sliderXml = juce::XmlDocument::parse(sliderSvg))
        sliderArtwork = juce::Drawable::createFromSVG(*sliderXml);
    const auto knobSvg = juce::String::fromUTF8(WaveAssets::Knob1_svg,
                                                WaveAssets::Knob1_svgSize);
    if (const auto knobXml = juce::XmlDocument::parse(knobSvg))
        knobArtwork = juce::Drawable::createFromSVG(*knobXml);
    const auto redKnobSvg = juce::String::fromUTF8(WaveAssets::Knob_red1_svg,
                                                   WaveAssets::Knob_red1_svgSize);
    if (const auto redKnobXml = juce::XmlDocument::parse(redKnobSvg))
        redKnobArtwork = juce::Drawable::createFromSVG(*redKnobXml);
    const auto largeRedKnobSvg = juce::String::fromUTF8(
        WaveAssets::Knob_red_large1_svg, WaveAssets::Knob_red_large1_svgSize);
    if (const auto largeRedKnobXml = juce::XmlDocument::parse(largeRedKnobSvg))
        largeRedKnobArtwork = juce::Drawable::createFromSVG(*largeRedKnobXml);
    const auto editButtonSvg = juce::String::fromUTF8(
        WaveAssets::button_edit_svg, WaveAssets::button_edit_svgSize);
    if (const auto editButtonXml = juce::XmlDocument::parse(editButtonSvg))
        editButtonArtwork = juce::Drawable::createFromSVG(*editButtonXml);
    const auto roundButtonSvg = juce::String::fromUTF8(
        WaveAssets::button_round_svg, WaveAssets::button_round_svgSize);
    if (const auto roundButtonXml = juce::XmlDocument::parse(roundButtonSvg))
        roundButtonArtwork = juce::Drawable::createFromSVG(*roundButtonXml);
    const auto verticalButtonSvg = juce::String::fromUTF8(
        WaveAssets::button_vertical_svg, WaveAssets::button_vertical_svgSize);
    if (const auto verticalButtonXml = juce::XmlDocument::parse(verticalButtonSvg))
        verticalButtonArtwork = juce::Drawable::createFromSVG(*verticalButtonXml);
    const auto ledOffSvg = juce::String::fromUTF8(
        WaveAssets::LED_OFF_svg, WaveAssets::LED_OFF_svgSize);
    if (const auto ledOffXml = juce::XmlDocument::parse(ledOffSvg))
        ledOffArtwork = juce::Drawable::createFromSVG(*ledOffXml);
    const auto ledOnGreenSvg = juce::String::fromUTF8(
        WaveAssets::LED_ON_GREEN_svg, WaveAssets::LED_ON_GREEN_svgSize);
    if (const auto ledOnGreenXml = juce::XmlDocument::parse(ledOnGreenSvg))
        ledOnGreenArtwork = juce::Drawable::createFromSVG(*ledOnGreenXml);
    const auto ledOnRedSvg = juce::String::fromUTF8(
        WaveAssets::LED_ON_RED_svg, WaveAssets::LED_ON_RED_svgSize);
    if (const auto ledOnRedXml = juce::XmlDocument::parse(ledOnRedSvg))
        ledOnRedArtwork = juce::Drawable::createFromSVG(*ledOnRedXml);
    const auto ledOnYellowSvg = juce::String::fromUTF8(
        WaveAssets::LED_ON_YELLOW_svg, WaveAssets::LED_ON_YELLOW_svgSize);
    if (const auto ledOnYellowXml = juce::XmlDocument::parse(ledOnYellowSvg))
        ledOnYellowArtwork = juce::Drawable::createFromSVG(*ledOnYellowXml);

    for (size_t index = 0; index < editButtonDesignCentres.size(); ++index)
    {
        auto button = std::make_unique<ArtworkButton>(editButtonArtwork.get());
        auto* buttonPointer = button.get();
        const auto hardwareId = editButtonHardwareIds[index];
        button->setTriggeredOnMouseDown(true);
        button->onStateChange = [this, buttonPointer, hardwareId] {
            const auto down = buttonPointer->getState() == juce::Button::buttonDown;
            ownerProcessor.setPanelButton(hardwareId, down);
        };
        // Match the physical panel: the press edge performs the action and
        // the release edge only returns the switch to its idle state.
        button->onClick = [this, hardwareId] {
            updateLocalButtonState(
                wave::panel::diagnosticCodeForMatrixIndex(hardwareId));
            repaint();
        };
        addAndMakeVisible(*button);
        editButtons.push_back(std::move(button));
    }
    synchroniseFaderValues();

    using Artwork = KnobArtworkType;
    using namespace wave::parameters;

    addKnob(oscillatorDetune[0], "Wave 1 Detune", { 156.0f, 81.0f });
    addKnob(modulationAmount[osc1PitchMod1], "Wave 1 Pitch Mod 1 Amount",
            { 235.0f, 81.0f });
    addKnob(oscillatorSemitone[0], "Wave 1 Semitone", { 156.0f, 160.0f });
    addKnob(modulationAmount[osc1PitchMod2], "Wave 1 Pitch Mod 2 Amount",
            { 235.0f, 160.0f });
    addKnob(wavePhase[0], "Wave 1 Start Phase", { 450.0f, 81.0f });
    addKnob(waveEnvelopeVelocity[0], "Wave 1 Envelope Velocity", { 508.0f, 81.0f });
    addKnob(modulationAmount[wave1Mod1], "Wave 1 Mod 1 Amount", { 646.0f, 81.0f });
    addKnob(position, "Wave 1 Start Wave", { 450.0f, 160.0f }, Artwork::standard);
    addKnob(scan, "Wave 1 Envelope Amount", { 508.0f, 160.0f });
    addKnob(waveKeytrack[0], "Wave 1 Keytrack", { 566.0f, 160.0f });
    addKnob(modulationAmount[wave1Mod2], "Wave 1 Mod 2 Amount", { 646.0f, 160.0f });

    addKnob(oscillatorDetune[1], "Wave 2 Detune", { 156.0f, 278.0f });
    addKnob(modulationAmount[osc2PitchMod1], "Wave 2 Pitch Mod 1 Amount",
            { 235.0f, 278.0f });
    addKnob(oscillatorSemitone[1], "Wave 2 Semitone", { 156.0f, 357.0f });
    addKnob(modulationAmount[osc2PitchMod2], "Wave 2 Pitch Mod 2 Amount",
            { 235.0f, 357.0f });
    addKnob(wavePhase[1], "Wave 2 Start Phase", { 450.0f, 278.0f });
    addKnob(waveEnvelopeVelocity[1], "Wave 2 Envelope Velocity", { 508.0f, 278.0f });
    addKnob(modulationAmount[wave2Mod1], "Wave 2 Mod 1 Amount", { 646.0f, 278.0f });
    addKnob(position2, "Wave 2 Start Wave", { 450.0f, 357.0f }, Artwork::standard);
    addKnob(scan2, "Wave 2 Envelope Amount", { 508.0f, 357.0f });
    addKnob(waveKeytrack[1], "Wave 2 Keytrack", { 566.0f, 357.0f });
    addKnob(modulationAmount[wave2Mod2], "Wave 2 Mod 2 Amount", { 646.0f, 357.0f });

    auto& dataDial = addKnob(wavetable, "Wavetable/Data", { 391.0f, 220.0f },
                             Artwork::largeRed, true);
    dataDial.setEncoderPixelsPerStep(6.0f);
    dataDial.setEncoderTurnCallback(
        [this](int steps) { ownerProcessor.turnPanelEncoder(8, steps); });
    addKnob(waveLevel[0], "Wave 1 Level", { 785.0f, 120.0f });
    addKnob(waveLevel[1], "Wave 2 Level", { 785.0f, 318.0f });
    addKnob(noise, "Noise Level", { 785.0f, 416.0f });
    // The Glide Rate pot is on the keyboard assembly. addKnob() stores upper-
    // panel-local X coordinates, hence 288 - panelHorizontalOffset here.
    addKnob(glideRate, "Glide Rate", { 222.0f, 753.0f });
    // System Volume is the second lower-controller knob. As with Glide Rate,
    // its centre is expressed relative to the upper-panel horizontal origin.
    addKnob(output, "System Volume", { 300.0f, 927.0f });

    lfoKnobs[0] = &addKnob(lfoRate[0], "LFO 1 Ratio", { 156.0f, 474.0f });
    lfoKnobs[1] = &addKnob(modulationAmount[lfo1RateMod], "LFO 1 Rate Mod Amount",
                           { 235.0f, 474.0f });
    lfoKnobs[2] = &addKnob(modulationAmount[lfo1LevelMod], "LFO 1 Level Mod Amount",
                           { 235.0f, 553.0f });

    constexpr std::array<float, 4> envelopeX { 450.0f, 508.0f, 566.0f, 627.0f };
    for (size_t point = 0; point < envelopeX.size(); ++point)
    {
        waveEnvelopeTimeKnobs[point]
            = &addKnob(waveEnvelopeTime[point],
                       "Wave Envelope Time " + juce::String(point + 1),
                       { envelopeX[point], 475.0f }, Artwork::standard, true);
        waveEnvelopeLevelKnobs[point]
            = &addKnob(waveEnvelopeLevel[point],
                       "Wave Envelope Level " + juce::String(point + 1),
                       { envelopeX[point], 554.0f }, Artwork::standard, true);
        waveEnvelopeTimeKnobs[point]->setEncoderTurnCallback(
            [this, point](int steps) {
                ownerProcessor.turnPanelEncoder(static_cast<int>(point), steps);
            });
        waveEnvelopeLevelKnobs[point]->setEncoderTurnCallback(
            [this, point](int steps) {
                ownerProcessor.turnPanelEncoder(static_cast<int>(point + 4), steps);
            });
    }

    addKnob(resonance, "Resonance", { 1604.0f, 159.0f });
    addKnob(filterVelocity, "Filter Envelope Velocity", { 1663.0f, 159.0f });
    addKnob(cutoff, "Cutoff", { 1604.0f, 237.0f }, Artwork::standard);
    addKnob(filterEnv, "Filter Envelope Amount", { 1663.0f, 237.0f });
    addKnob(filterKeytrack, "Filter Keytrack", { 1721.0f, 237.0f });
    addKnob(modulationAmount[resonanceMod], "Resonance Mod Amount", { 1799.0f, 81.0f });
    addKnob(modulationAmount[filterMod1], "Cutoff Mod 1 Amount", { 1799.0f, 159.0f });
    addKnob(modulationAmount[filterMod2], "Cutoff Mod 2 Amount", { 1799.0f, 237.0f });
    addKnob(filterDelay, "Filter Envelope Delay", { 1486.0f, 354.0f });
    addKnob(filterAttack, "Filter Envelope Attack", { 1545.0f, 354.0f });
    addKnob(filterDecay, "Filter Envelope Decay", { 1604.0f, 354.0f });
    addKnob(filterSustain, "Filter Envelope Sustain", { 1663.0f, 354.0f });
    addKnob(filterRelease, "Filter Envelope Release", { 1721.0f, 354.0f });

    addKnob(highpassVelocity, "High-pass Envelope Velocity", { 1956.0f, 159.0f });
    addKnob(highpassEnvelopeAmount, "High-pass Envelope Amount", { 1956.0f, 237.0f });
    addKnob(highpassKeytrack, "High-pass Keytrack", { 2015.0f, 237.0f });
    addKnob(modulationAmount[highpassMod1], "High-pass Mod 1 Amount", { 2093.0f, 159.0f });
    addKnob(modulationAmount[highpassMod2], "High-pass Mod 2 Amount", { 2093.0f, 237.0f });

    addKnob(attack, "VCA Attack", { 1897.0f, 354.0f });
    addKnob(decay, "VCA Decay", { 1956.0f, 354.0f });
    addKnob(sustain, "VCA Sustain", { 2015.0f, 354.0f });
    addKnob(release, "VCA Release", { 2073.0f, 354.0f });
    addKnob(modulationAmount[panMod1], "Panning Mod 1 Amount", { 1897.0f, 473.0f });
    addKnob(modulationAmount[panMod2], "Panning Mod 2 Amount", { 1897.0f, 551.0f });

    lcd = std::make_unique<wave::ui::WaveLcdComponent>(ownerProcessor);
    lcd->setPanelEmbedded(true);
    addAndMakeVisible(*lcd);

    setWindowScale(defaultEditorScale);
    startTimerHz(60);
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer(this)] {
        if (safe != nullptr)
            safe->grabKeyboardFocus();
    });
}

WaveEmulationAudioProcessorEditor::~WaveEmulationAudioProcessorEditor()
{
    stopTimer();
    // Pitch Bend is the sole spring-loaded controller. Mod and the freely
    // assignable bipolar Free wheel retain their physical positions.
    if (activePerformanceWheel == 0)
        ownerProcessor.setPitchWheelFromUi(8192.0f / 16383.0f);
    releaseComputerKeyboardShift();
    ownerProcessor.allSoundOffFromUi();
    for (const auto hardwareId : editButtonHardwareIds)
        ownerProcessor.setPanelButton(hardwareId, false);
    if (activePanelButton >= 0)
        ownerProcessor.setPanelButton(activePanelButton, false);
    if (activePanelFader >= 0)
        ownerProcessor.endPanelFaderGesture(activePanelFader,
                                             activePanelFaderPerformanceMode);
    setLookAndFeel(nullptr);
}

juce::StringArray WaveEmulationAudioProcessorEditor::getMenuBarNames()
{
    return { "System" };
}

juce::PopupMenu WaveEmulationAudioProcessorEditor::getMenuForIndex(
    int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;
    if (topLevelMenuIndex != 0)
        return menu;

    menu.addItem(loadFirmwareMenuItem, "Load System Firmware Folder...");
    menu.addItem(voiceAllocationFixMenuItem, "OS 1.700 Voice Allocation Fix (no stolen held notes)", true,
                 ownerProcessor.getVoiceAllocationFix());
    menu.addSeparator();
    const auto mounted = ownerProcessor.hasMountedDiskImage();
    menu.addItem(createBlankDiskMenuItem, "New Blank 720 KB DD Disk Image...");
    menu.addItem(createDiskFromSetMenuItem, "Create Disk Image from Wave Setup...");
    menu.addSeparator();
    menu.addItem(mountDiskMenuItem, mounted ? "Mount Another Disk Image..."
                                            : "Mount Disk Image...");
    menu.addItem(saveDiskMenuItem, "Save Mounted Disk Image", mounted
                                                              && ownerProcessor
                                                                     .mountedDiskImageIsWritable());
    menu.addItem(saveDiskAsMenuItem, "Save Mounted Disk Image As...", mounted);
    menu.addItem(ejectDiskMenuItem, "Eject Disk", mounted);
    menu.addSeparator();
    menu.addItem(-1, ownerProcessor.getMountedDiskDescription(), false, false);
    menu.addSeparator();
    menu.addItem(zoomInMenuItem, "Zoom In (Cmd/Ctrl + =)");
    menu.addItem(zoomOutMenuItem, "Zoom Out (Cmd/Ctrl + -)");
    menu.addItem(resetZoomMenuItem, "Actual Size (Cmd/Ctrl + 0)");
    menu.addItem(toggleKeyboardMenuItem,
                 keyboardVisible ? "Hide Lower Keyboard Area (Cmd/Ctrl + K)"
                                 : "Show Lower Keyboard Area (Cmd/Ctrl + K)",
                 !tabbedLayout);
    menu.addItem(tabbedLayoutMenuItem, "Tabbed Layout", true, tabbedLayout);
    menu.addSeparator();
    juce::PopupMenu skins;
    skins.addItem(defaultPanelSkinMenuItem, "Original", true, panelSkinFile == juce::File{});
    skins.addItem(loadPanelSkinMenuItem, "Load Alternative SVG Skin...");
    if (panelSkinFile != juce::File{})
        skins.addItem(-2, "Current: " + panelSkinFile.getFileName(), false);
    menu.addSubMenu("Panel Skin", skins);
    return menu;
}

void WaveEmulationAudioProcessorEditor::showSystemMenu()
{
    if (systemMenuButton == nullptr)
        return;
    auto menu = getMenuForIndex(0, "System");
    menu.showMenuAsync(
        juce::PopupMenu::Options {}.withTargetComponent(systemMenuButton.get()),
        [safe = juce::Component::SafePointer(this)](int itemId) {
            if (safe != nullptr && itemId != 0)
                safe->menuItemSelected(itemId, 0);
        });
}

void WaveEmulationAudioProcessorEditor::menuItemSelected(int menuItemId, int)
{
    if (menuItemId == loadPanelSkinMenuItem)
    {
        showPanelSkinChooser();
        return;
    }
    if (menuItemId == defaultPanelSkinMenuItem)
    {
        useDefaultPanelSkin();
        return;
    }
    if (menuItemId == tabbedLayoutMenuItem)
    {
        setTabbedLayout(!tabbedLayout);
        return;
    }
    if (menuItemId == zoomInMenuItem || menuItemId == zoomOutMenuItem)
    {
        const auto currentScale = viewScale();
        const auto direction = menuItemId == zoomInMenuItem ? 1.0f : -1.0f;
        setWindowScale(std::round(currentScale * 10.0f + direction) / 10.0f);
        return;
    }
    if (menuItemId == resetZoomMenuItem)
    {
        setWindowScale(defaultEditorScale);
        return;
    }
    if (menuItemId == toggleKeyboardMenuItem)
    {
        setKeyboardVisible(!keyboardVisible);
        return;
    }
    if (menuItemId == loadFirmwareMenuItem)
    {
        showFirmwareFolderChooser();
        return;
    }
    if (menuItemId == voiceAllocationFixMenuItem)
    {
        ownerProcessor.setVoiceAllocationFix(!ownerProcessor.getVoiceAllocationFix());
        menuItemsChanged();
        return;
    }
    if (menuItemId == createBlankDiskMenuItem)
    {
        showCreateBlankDiskChooser();
        return;
    }
    if (menuItemId == createDiskFromSetMenuItem)
    {
        showCreateDiskFromSetChooser();
        return;
    }
    if (menuItemId == mountDiskMenuItem)
    {
        showDiskImageChooser();
        return;
    }
    if (menuItemId == saveDiskMenuItem)
    {
        const auto result = ownerProcessor.flushMountedDiskImage();
        if (result.failed())
            showDiskError("Could not save disk image", result);
        menuItemsChanged();
        return;
    }
    if (menuItemId == saveDiskAsMenuItem)
    {
        showSaveDiskAsChooser();
        return;
    }
    if (menuItemId == ejectDiskMenuItem)
    {
        const auto result = ownerProcessor.ejectDiskImage();
        if (result.failed())
            showDiskError("Could not eject disk image", result);
        menuItemsChanged();
    }
}

void WaveEmulationAudioProcessorEditor::setWindowScale(float scale)
{
    scale = juce::jlimit(minimumEditorScale, maximumEditorScale, scale);
    const auto view = viewSize();
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio(static_cast<double>(view.x / view.y));
    setResizeLimits(juce::roundToInt(view.x * minimumEditorScale),
                    juce::roundToInt(view.y * minimumEditorScale),
                    juce::roundToInt(view.x * maximumEditorScale),
                    juce::roundToInt(view.y * maximumEditorScale));
    setSize(juce::roundToInt(view.x * scale), juce::roundToInt(view.y * scale));
}

void WaveEmulationAudioProcessorEditor::setTabbedLayout(bool tabbed, bool remember)
{
    if (remember)
        ownerProcessor.rememberTabbedLayout(tabbed);
    if (tabbedLayout == tabbed)
        return;
    releaseActivePointerInteractions(false);
    const auto currentScale = viewScale();
    tabbedLayout = tabbed;
    setWindowScale(currentScale);
    menuItemsChanged();
}

void WaveEmulationAudioProcessorEditor::selectTab(int tab)
{
    tab = juce::jlimit(0, static_cast<int>(tabNames.size()) - 1, tab);
    if (selectedTab == tab)
        return;
    releaseActivePointerInteractions(false);
    selectedTab = tab;
    layoutComponents();
    repaint();
}

float WaveEmulationAudioProcessorEditor::viewScale() const noexcept
{
    return static_cast<float>(getWidth()) / viewSize().x;
}

juce::Point<float> WaveEmulationAudioProcessorEditor::viewSize() const noexcept
{
    if (tabbedLayout)
        return { tabbedViewWidth, tabbedViewHeight };
    return { designWidth, keyboardVisible ? designHeight : panelOnlyHeight };
}

juce::Point<float> WaveEmulationAudioProcessorEditor::viewToDesign(
    juce::Point<float> viewPoint) const noexcept
{
    if (getWidth() <= 0)
        return { -1.0e6f, -1.0e6f };
    const auto point = viewPoint / viewScale();
    const auto& blocks = layoutBlocksFor(tabbedLayout, selectedTab);
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block)
        if (block->destinationBounds().contains(point))
            return block->source.getPosition() + (point - block->destination);
    // Outside every block: a point far off the artwork that hits no control.
    return { -1.0e6f, -1.0e6f };
}

std::optional<juce::Rectangle<int>> WaveEmulationAudioProcessorEditor::designToView(
    juce::Rectangle<float> designBounds) const noexcept
{
    const auto& blocks = layoutBlocksFor(tabbedLayout, selectedTab);
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block)
        if (block->source.contains(designBounds.getCentre()))
            return ((designBounds + (block->destination - block->source.getPosition()))
                    * viewScale()).toNearestInt();
    return std::nullopt;
}

void WaveEmulationAudioProcessorEditor::setKeyboardVisible(bool visible)
{
    if (keyboardVisible == visible)
        return;
    releaseActivePointerInteractions(false);
    const auto currentScale = viewScale();
    keyboardVisible = visible;
    setWindowScale(currentScale);
    menuItemsChanged();
}

void WaveEmulationAudioProcessorEditor::showFirmwareFolderChooser()
{
    firmwareFolderChooser = std::make_unique<juce::FileChooser>(
        "Select the folder containing w2sys.bin and wdv.sys",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
    firmwareFolderChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safe = juce::Component::SafePointer(this)](const juce::FileChooser& chooser) {
            if (safe == nullptr || chooser.getResult() == juce::File{})
                return;
            wave::firmware::Bundle candidate;
            auto report = candidate.load(chooser.getResult());
            if (report.hasBothImages())
            {
                auto& processor = safe->ownerProcessor;
                const juce::ScopedLock callbackLock(processor.getCallbackLock());
                report = processor.loadFirmware(chooser.getResult());
            }
            juce::AlertWindow::showMessageBoxAsync(
                report.hasBothImages() ? juce::MessageBoxIconType::InfoIcon
                                       : juce::MessageBoxIconType::WarningIcon,
                report.summary, report.detail);
        });
}

void WaveEmulationAudioProcessorEditor::showCreateBlankDiskChooser()
{
    const auto suggested = juce::File::getSpecialLocation(
                               juce::File::userDocumentsDirectory)
                               .getChildFile("Blank Wave Disk.img");
    diskDestinationChooser = std::make_unique<juce::FileChooser>(
        "Create a blank 720 KB DD Wave disk image", suggested, "*.img", true);
    const auto flags = juce::FileBrowserComponent::saveMode
                       | juce::FileBrowserComponent::canSelectFiles
                       | juce::FileBrowserComponent::warnAboutOverwriting;
    diskDestinationChooser->launchAsync(
        flags, [safe = juce::Component::SafePointer(this)](
                   const juce::FileChooser& chooser) {
            if (safe == nullptr)
                return;
            const auto destination = chooser.getResult();
            if (destination == juce::File{})
                return;
            const auto result = safe->ownerProcessor.createBlankDiskImage(destination);
            if (result.failed())
                safe->showDiskError("Could not create blank Wave disk image", result);
            else
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::InfoIcon, "Blank Wave disk ready",
                    "A formatted 720 KB DD disk image was created and mounted.");
            safe->menuItemsChanged();
        });
}

void WaveEmulationAudioProcessorEditor::showCreateDiskFromSetChooser()
{
    waveSetChooser = std::make_unique<juce::FileChooser>(
        "Choose a Waldorf Wave Setup", juce::File::getSpecialLocation(
                                             juce::File::userDocumentsDirectory),
        "*.set", true);
    const auto flags = juce::FileBrowserComponent::openMode
                       | juce::FileBrowserComponent::canSelectFiles;
    waveSetChooser->launchAsync(
        flags, [safe = juce::Component::SafePointer(this)](const juce::FileChooser& chooser) {
            if (safe == nullptr)
                return;
            const auto selected = chooser.getResult();
            if (selected.existsAsFile())
                safe->showDiskDestinationChooser(selected);
        });
}

void WaveEmulationAudioProcessorEditor::showDiskDestinationChooser(
    const juce::File& waveSetup)
{
    const auto suggested = waveSetup.getSiblingFile(
        waveSetup.getFileNameWithoutExtension() + ".img");
    diskDestinationChooser = std::make_unique<juce::FileChooser>(
        "Save the Wave floppy image", suggested, "*.img", true);
    const auto flags = juce::FileBrowserComponent::saveMode
                       | juce::FileBrowserComponent::canSelectFiles
                       | juce::FileBrowserComponent::warnAboutOverwriting;
    diskDestinationChooser->launchAsync(
        flags, [safe = juce::Component::SafePointer(this), waveSetup](
                   const juce::FileChooser& chooser) {
            if (safe == nullptr)
                return;
            const auto destination = chooser.getResult();
            if (destination == juce::File{})
                return;
            const auto result = safe->ownerProcessor.createDiskImageFromWaveSetup(
                waveSetup, destination);
            if (result.failed())
                safe->showDiskError("Could not create Wave disk image", result);
            else
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::InfoIcon, "Wave disk ready",
                    "The canonical 720 KB DD disk image was created and mounted.");
            safe->menuItemsChanged();
        });
}

void WaveEmulationAudioProcessorEditor::showSaveDiskAsChooser()
{
    const auto mounted = ownerProcessor.getMountedDiskImageFile();
    if (mounted == juce::File{})
        return;
    const auto suggested = mounted.getSiblingFile(
        mounted.getFileNameWithoutExtension() + " Copy.img");
    diskDestinationChooser = std::make_unique<juce::FileChooser>(
        "Save a copy of the mounted Wave disk image", suggested, "*.img", true);
    const auto flags = juce::FileBrowserComponent::saveMode
                       | juce::FileBrowserComponent::canSelectFiles
                       | juce::FileBrowserComponent::warnAboutOverwriting;
    diskDestinationChooser->launchAsync(
        flags, [safe = juce::Component::SafePointer(this)](
                   const juce::FileChooser& chooser) {
            if (safe == nullptr)
                return;
            const auto destination = chooser.getResult();
            if (destination == juce::File{})
                return;
            const auto result = safe->ownerProcessor.saveMountedDiskImageAs(destination);
            if (result.failed())
                safe->showDiskError("Could not save disk image copy", result);
            safe->menuItemsChanged();
        });
}

void WaveEmulationAudioProcessorEditor::showDiskImageChooser()
{
    const auto initial = ownerProcessor.getDiskImageChooserDirectory();
    diskImageChooser = std::make_unique<juce::FileChooser>(
        "Mount a raw MS-DOS floppy image", initial, "*.img;*.ima;*.dsk;*.st", true);
    const auto flags = juce::FileBrowserComponent::openMode
                       | juce::FileBrowserComponent::canSelectFiles;
    diskImageChooser->launchAsync(
        flags, [safe = juce::Component::SafePointer(this)](const juce::FileChooser& chooser) {
            if (safe == nullptr)
                return;
            const auto selected = chooser.getResult();
            if (selected == juce::File{})
                return;
            const auto result = safe->ownerProcessor.mountDiskImage(selected);
            if (result.failed())
                safe->showDiskError("Could not mount disk image", result);
            safe->menuItemsChanged();
        });
}

void WaveEmulationAudioProcessorEditor::showDiskError(const juce::String& title,
                                                       const juce::Result& result)
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                           title, result.getErrorMessage());
}

bool WaveEmulationAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    const auto code = juce::CharacterFunctions::toLowerCase(key.getKeyCode());
    if (key.getModifiers().isCommandDown())
    {
        if (code == '+' || code == '=' || code == juce::KeyPress::numberPadAdd)
        {
            menuItemSelected(zoomInMenuItem, 0);
            return true;
        }
        if (code == '-' || code == juce::KeyPress::numberPadSubtract)
        {
            menuItemSelected(zoomOutMenuItem, 0);
            return true;
        }
        if (code == '0' || code == juce::KeyPress::numberPad0)
        {
            menuItemSelected(resetZoomMenuItem, 0);
            return true;
        }
        if (code == 'k')
        {
            menuItemSelected(toggleKeyboardMenuItem, 0);
            return true;
        }
    }
    const auto found = std::find(performanceKeys.begin(), performanceKeys.end(), code);
    if (found == performanceKeys.end())
        return false;

    const auto index = static_cast<size_t>(std::distance(performanceKeys.begin(), found));
    if (!keyboardNotes[index])
    {
        keyboardNotes[index] = true;
        ownerProcessor.noteOnFromUi(60 + static_cast<int>(index));
    }
    return true;
}

bool WaveEmulationAudioProcessorEditor::keyStateChanged(bool)
{
    const auto handledNotes = updateComputerKeyboardNotes();
    const auto handledShift = updateComputerKeyboardShift();
    return handledNotes || handledShift;
}

bool WaveEmulationAudioProcessorEditor::updateComputerKeyboardNotes()
{
    auto handled = false;
    const auto commandDown
        = juce::ModifierKeys::getCurrentModifiersRealtime().isCommandDown();
    for (size_t index = 0; index < performanceKeys.size(); ++index)
    {
        const auto down
            = !commandDown
              && (juce::KeyPress::isKeyCurrentlyDown(performanceKeys[index])
                  || juce::KeyPress::isKeyCurrentlyDown(
                      juce::CharacterFunctions::toUpperCase(performanceKeys[index])));
        if (down == keyboardNotes[index])
            continue;

        keyboardNotes[index] = down;
        const auto midiNote = 60 + static_cast<int>(index);
        if (down)
            ownerProcessor.noteOnFromUi(midiNote);
        else
        {
            ownerProcessor.noteOffFromUi(midiNote);
            recentMidiNote = midiNote;
            recentMidiNoteHighlightUntil = juce::Time::getMillisecondCounterHiRes()
                                           + recentMidiFeedbackMilliseconds;
        }
        handled = true;
    }
    return handled;
}

void WaveEmulationAudioProcessorEditor::releaseComputerKeyboardNotes()
{
    if (std::none_of(keyboardNotes.begin(), keyboardNotes.end(), [](bool down) { return down; }))
        return;
    keyboardNotes.fill(false);
    ownerProcessor.allSoundOffFromUi();
}

bool WaveEmulationAudioProcessorEditor::updateComputerKeyboardShift()
{
    const auto down = juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
    if (down == computerShiftDown)
        return false;
    computerShiftDown = down;
    ownerProcessor.setKeyboardControllerButton(0x52u, down);
    return true;
}

void WaveEmulationAudioProcessorEditor::releaseComputerKeyboardShift()
{
    if (!computerShiftDown)
        return;
    computerShiftDown = false;
    ownerProcessor.setKeyboardControllerButton(0x52u, false);
}

void WaveEmulationAudioProcessorEditor::focusLost(FocusChangeType)
{
    if (!hasKeyboardFocus(true))
    {
        releaseActivePointerInteractions(false);
        releaseComputerKeyboardNotes();
        releaseComputerKeyboardShift();
    }
}

void WaveEmulationAudioProcessorEditor::paint(juce::Graphics& graphics)
{
    const auto scale = viewScale();
    if (!tabbedLayout)
    {
        graphics.fillAll(juce::Colours::black);
        paintDesign(graphics, scale);
        return;
    }

    const auto panelColour = juce::Colour(0xff2e2d45);
    const auto headerColour = tabHeaderColour;
    graphics.fillAll(panelColour);
    for (const auto& block : layoutBlocksFor(true, selectedTab))
    {
        juce::Graphics::ScopedSaveState saved(graphics);
        graphics.reduceClipRegion((block.destinationBounds() * scale).toNearestInt());
        const auto offset = (block.destination - block.source.getPosition()) * scale;
        graphics.addTransform(juce::AffineTransform::translation(offset.x, offset.y));
        paintDesign(graphics, scale);
    }

    // Same colour and 4 px weight as the frame lines rising from the screen.
    graphics.setColour(panelDividerColour);
    graphics.fillRect(juce::Rectangle<float> { 0.0f, tabStripHeight, tabbedViewWidth, 4.0f }
                      * scale);

    graphics.setFont(juce::FontOptions(15.0f * scale, juce::Font::bold | juce::Font::italic));
    for (auto tab = 0; tab < static_cast<int>(tabNames.size()); ++tab)
    {
        const auto bounds = tabBounds(tab) * scale;
        const auto selected = tab == selectedTab;
        graphics.setColour(selected ? headerColour : headerColour.withAlpha(0.18f));
        graphics.fillRoundedRectangle(bounds, 3.0f * scale);
        graphics.setColour(selected ? panelColour : headerColour);
        graphics.drawText(tabNames[static_cast<size_t>(tab)], bounds,
                          juce::Justification::centred);
    }
}

void WaveEmulationAudioProcessorEditor::paintDesign(juce::Graphics& graphics, float scale)
{
    const auto scaleX = scale;
    const auto fullPanelBounds = juce::Rectangle<float> {
        0.0f, 0.0f, designWidth * scaleX, designHeight * scaleX
    };
    if (panelImage.isValid())
    {
        graphics.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        graphics.drawImage(panelImage, fullPanelBounds,
                           juce::RectanglePlacement::stretchToFit);
    }
    else if (panelArtwork != nullptr)
        panelArtwork->drawWithin(graphics, fullPanelBounds,
                                 juce::RectanglePlacement::stretchToFit, 1.0f);

    const auto scaleY = scaleX;
    const auto minScale = juce::jmin(scaleX, scaleY);
    const auto scaledKeyboardBounds = [scaleX, scaleY](juce::Rectangle<float> bounds) {
        return juce::Rectangle<float> {
            bounds.getX() * scaleX, bounds.getY() * scaleY,
            bounds.getWidth() * scaleX, bounds.getHeight() * scaleY
        };
    };

    // Draw the keyboard at runtime so it remains visible even when the panel
    // SVG is re-exported with only the empty black keyboard bed. The keybed
    // deliberately occupies the complete 1671 x 278 black rectangle.
    const auto whiteKeys = scaledKeyboardBounds(
        { keyboardLeft, keyboardTop, keyboardWidth, keyboardWhiteKeyHeight });

    juce::ColourGradient whiteKeyGradient(
        juce::Colour(0xfff8f8f5), whiteKeys.getX(), whiteKeys.getY(),
        juce::Colour(0xffd3d3cf), whiteKeys.getX(), whiteKeys.getBottom(), false);
    whiteKeyGradient.addColour(0.78, juce::Colour(0xffe9e9e5));
    graphics.setGradientFill(whiteKeyGradient);
    graphics.fillRect(whiteKeys);

    graphics.setColour(juce::Colour(0xff27282c));
    for (auto whiteKey = 1; whiteKey < 36; ++whiteKey)
    {
        const auto x = (keyboardLeft
                        + static_cast<float>(whiteKey) * keyboardWhiteKeyWidth) * scaleX;
        graphics.drawLine(x, whiteKeys.getY(), x, whiteKeys.getBottom(),
                          juce::jmax(1.5f, 2.0f * minScale));
    }

    const auto feedbackIsVisible = recentMidiNote >= 0
                                   && juce::Time::getMillisecondCounterHiRes()
                                          < recentMidiNoteHighlightUntil;
    const auto drawMidiKeyFeedback = [&](int midiNote, bool blackKeys) {
        if (midiNote < keyboardLowestMidiNote || midiNote > keyboardHighestMidiNote
            || isBlackMidiNote(midiNote) != blackKeys)
            return;
        const auto bounds = scaledKeyboardBounds(midiKeyBounds(midiNote));
        graphics.setColour(juce::Colour(0xffda2e2e).withAlpha(0.25f));
        if (blackKeys)
            graphics.fillRoundedRectangle(bounds, 2.0f * minScale);
        else
            graphics.fillRect(bounds);
    };
    const auto drawFeedbackForKeyType = [&](bool blackKeys) {
        for (auto midiNote = keyboardLowestMidiNote;
             midiNote <= keyboardHighestMidiNote; ++midiNote)
            if (ownerProcessor.isMidiNoteActive(midiNote))
                drawMidiKeyFeedback(midiNote, blackKeys);
        for (size_t index = 0; index < keyboardNotes.size(); ++index)
            if (keyboardNotes[index])
                drawMidiKeyFeedback(60 + static_cast<int>(index), blackKeys);
        if (feedbackIsVisible)
            drawMidiKeyFeedback(recentMidiNote, blackKeys);
        if (activeMidiNote >= 0)
            drawMidiKeyFeedback(activeMidiNote, blackKeys);
    };

    // White-key feedback is painted before the raised black keys so adjacent
    // black keys retain their physical overlap and remain easy to read.
    drawFeedbackForKeyType(false);

    juce::ColourGradient blackKeyGradient(
        juce::Colour(0xff383a42), 0.0f, keyboardTop * scaleY,
        juce::Colour(0xff050506), 0.0f,
        (keyboardTop + keyboardBlackKeyHeight) * scaleY, false);
    blackKeyGradient.addColour(0.18, juce::Colour(0xff202127));
    blackKeyGradient.addColour(0.82, juce::Colour(0xff111216));
    for (auto midiNote = keyboardLowestMidiNote;
         midiNote <= keyboardHighestMidiNote; ++midiNote)
    {
        if (!isBlackMidiNote(midiNote))
            continue;
        const auto bounds = scaledKeyboardBounds(midiKeyBounds(midiNote));
        graphics.setGradientFill(blackKeyGradient);
        graphics.fillRoundedRectangle(bounds, 2.0f * minScale);
        graphics.setColour(juce::Colour(0xff050506));
        graphics.drawRoundedRectangle(bounds, 2.0f * minScale,
                                      juce::jmax(1.0f, 2.0f * minScale));
    }
    drawFeedbackForKeyType(true);

    // The SVG supplies the three exact black wheel recesses. Draw only the
    // black wheel bodies inside those bounds, preserving the Figma layout and
    // adjacent position markings unchanged.
    for (size_t wheel = 0; wheel < performanceWheelSlots.size(); ++wheel)
    {
        const auto slot = scaledKeyboardBounds(performanceWheelSlots[wheel]);
        const auto centreY = juce::jmap(
            performanceWheelValues[wheel], slot.getBottom() - 13.0f * scaleY,
            slot.getY() + 13.0f * scaleY);
        const auto body = juce::Rectangle<float> {
            slot.getWidth() - 2.0f * scaleX, 34.0f * scaleY
        }.withCentre({ slot.getCentreX(), centreY });
        juce::Graphics::ScopedSaveState clipped(graphics);
        graphics.reduceClipRegion(slot.toNearestInt());
        juce::ColourGradient wheelGradient(
            juce::Colour(0xff050506), body.getX(), body.getY(),
            juce::Colour(0xff202126), body.getRight(), body.getY(), false);
        wheelGradient.addColour(0.5, juce::Colour(0xff0b0c0f));
        graphics.setGradientFill(wheelGradient);
        graphics.fillRoundedRectangle(body, 3.0f * minScale);
        graphics.setColour(juce::Colour(0xff363840));
        for (auto rib = 1; rib < 6; ++rib)
        {
            const auto y = body.getY()
                           + static_cast<float>(rib) * body.getHeight() / 6.0f;
            graphics.drawLine(body.getX() + 2.0f * scaleX, y,
                              body.getRight() - 2.0f * scaleX, y,
                              juce::jmax(0.6f, 0.8f * minScale));
        }
        graphics.setColour(juce::Colours::black);
        graphics.drawRoundedRectangle(body, 3.0f * minScale,
                                      juce::jmax(1.0f, minScale));
    }

    if (roundButtonArtwork != nullptr)
    {
        for (const auto& region : panelRegions)
        {
            if (!region.roundButton)
                continue;
            roundButtonArtwork->drawWithin(
                graphics,
                juce::Rectangle<float> { 30.0f * scaleX, 30.0f * scaleY }
                    .withCentre({ region.centre.x * scaleX, region.centre.y * scaleY }),
                juce::RectanglePlacement::stretchToFit, 1.0f);
        }
    }
    if (verticalButtonArtwork != nullptr)
    {
        for (const auto& region : panelRegions)
        {
            if (!region.verticalButton)
                continue;
            verticalButtonArtwork->drawWithin(
                graphics,
                juce::Rectangle<float> { 30.0f * scaleX, 61.0f * scaleY }
                    .withCentre({ region.centre.x * scaleX, region.centre.y * scaleY }),
                juce::RectanglePlacement::stretchToFit, 1.0f);
        }
    }
    if (sliderArtwork != nullptr)
    {
        constexpr auto handleWidth = 26.0f;
        constexpr auto handleHeight = 50.0f;
        for (const auto& region : panelRegions)
        {
            if (!region.fader)
                continue;
            const auto value = panelAnalogValues[static_cast<size_t>(region.hardwareId)];
            const auto handleX = region.centre.x - handleWidth * 0.5f;
            const auto handleCentreY = juce::jmap(value, region.faderTrackBottom,
                                                  region.faderTrackTop);
            const auto handleY = handleCentreY - handleHeight * 0.5f;
            sliderArtwork->drawWithin(
                graphics,
                { handleX * scaleX, handleY * scaleY,
                  handleWidth * scaleX, handleHeight * scaleY },
                juce::RectanglePlacement::stretchToFit, 1.0f);
        }
    }
    if (knobArtwork != nullptr && knobArtworkTypes.size() == knobs.size())
    {
        // Each supplied SVG is drawn unchanged. Only the complete asset rotates
        // around its panel centre as the attached parameter moves.
        constexpr auto rotationExtent = juce::MathConstants<float>::pi * 0.75f;
        for (size_t index = 0; index < knobs.size(); ++index)
        {
            const auto centre = knobDesignCentres[index].translated(
                                    panelHorizontalOffset, 0.0f) * scaleX;
            auto* artwork = knobArtwork.get();
            auto artworkSize = 34.0f;
            if (knobArtworkTypes[index] == KnobArtworkType::red)
            {
                artwork = redKnobArtwork.get();
                artworkSize = 35.0f;
            }
            else if (knobArtworkTypes[index] == KnobArtworkType::largeRed)
            {
                artwork = largeRedKnobArtwork.get();
                artworkSize = 90.0f;
            }
            if (artwork == nullptr)
                continue;
            const auto artworkBounds = juce::Rectangle<float> {
                artworkSize * scaleX, artworkSize * scaleY
            }.withCentre(centre);
            const auto angle = knobs[index]->artworkAngle(rotationExtent);
            juce::Graphics::ScopedSaveState saved(graphics);
            graphics.addTransform(
                juce::AffineTransform::rotation(angle, centre.x, centre.y));
            artwork->drawWithin(graphics, artworkBounds,
                                juce::RectanglePlacement::stretchToFit, 1.0f);
        }
    }
    for (const auto& led : panelLeds)
    {
        auto* artwork = ledOffArtwork.get();
        const auto colour = panelLedColour(led);
        if (colour == juce::Colours::green && ledOnGreenArtwork != nullptr)
            artwork = ledOnGreenArtwork.get();
        else if (colour == juce::Colours::red && ledOnRedArtwork != nullptr)
            artwork = ledOnRedArtwork.get();
        else if (colour == juce::Colours::yellow && ledOnYellowArtwork != nullptr)
            artwork = ledOnYellowArtwork.get();
        const auto bounds = juce::Rectangle<float> { 6.0f * scaleX, 6.0f * scaleY }
                                .withCentre({ led.centre.x * scaleX,
                                              led.centre.y * scaleY });
        if (artwork != nullptr)
            artwork->drawWithin(graphics, bounds,
                                juce::RectanglePlacement::stretchToFit, 1.0f);
    }
}

void WaveEmulationAudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    if (tabbedLayout)
        for (auto tab = 0; tab < static_cast<int>(tabNames.size()); ++tab)
            if ((tabBounds(tab) * viewScale()).contains(event.position))
            {
                selectTab(tab);
                return;
            }
    const auto designPoint = viewToDesign(event.position);
    if (const auto midiNote = midiNoteAt(designPoint); midiNote >= 0)
    {
        midiKeyboardDragging = true;
        recentMidiNote = -1;
        activeMidiNote = midiNote;
        ownerProcessor.noteOnFromUi(activeMidiNote);
        repaint();
        return;
    }

    for (size_t wheel = 0; wheel < performanceWheelSlots.size(); ++wheel)
    {
        if (!performanceWheelSlots[wheel].contains(designPoint))
            continue;
        activePerformanceWheel = static_cast<int>(wheel);
        analogDragStartY = event.position.y;
        analogDragStartValue = performanceWheelValues[wheel];
        analogDragRangePixels = performanceWheelSlots[wheel].getHeight()
                                * viewScale();
        return;
    }

    const auto panelPoint = designPoint.translated(-panelHorizontalOffset, 0.0f);
    if (hitCircle(panelPoint, 39.0f, 555.0f))
    {
        pressPanelButton(wave::panel::matrixIndexForDiagnosticCode(2));
        updateLfoKnobBindings();
        repaint();
        return;
    }
    if (hitCircle(panelPoint, 392.0f, 555.0f))
    {
        pressPanelButton(wave::panel::matrixIndexForDiagnosticCode(14));
        updateWaveEnvelopeKnobBindings();
        repaint();
        return;
    }
    if (handlePerformanceControl(panelPoint))
        return;

    // The seven blue edit-mode selectors above Performance leave Performance
    // mode. Their normal firmware button handling remains unchanged below.
    if (hitRectangle(panelPoint, 1372.0f, 124.0f, 61.0f, 385.0f))
        performanceMode = false;

    for (auto region = panelRegions.rbegin(); region != panelRegions.rend(); ++region)
    {
        if (region->path.contains(designPoint.x, designPoint.y))
        {
            if (region->analog)
            {
                if (region->hardwareId < 0)
                    continue;
                activePanelAnalog = region->hardwareId;
                analogDragStartY = event.position.y;
                analogDragStartValue = panelAnalogValues[static_cast<size_t>(activePanelAnalog)];
                analogDragRangePixels = region->fader
                                            ? juce::jmax(
                                                  1.0f,
                                                  (region->faderTrackBottom
                                                   - region->faderTrackTop)
                                                      * viewScale())
                                            : 160.0f;
                if (region->fader)
                {
                    activePanelFader = region->faderIndex;
                    // An Edit overlay makes the eight display faders
                    // contextual even when the underlying blue operating mode
                    // remains Performance (the state shown in the report).
                    activePanelFaderPerformanceMode
                        = performanceMode && selectedEditSwitch < 0;
                    ownerProcessor.beginPanelFaderGesture(activePanelFader,
                                                           activePanelFaderPerformanceMode);
                }
                return;
            }
            if (region->keyboardCode >= 0)
            {
                activeKeyboardButton = region->keyboardCode;
                ownerProcessor.setKeyboardControllerButton(
                    static_cast<uint8_t>(activeKeyboardButton), true);
                return;
            }
            pressPanelButton(region->hardwareId);
            return;
        }
    }
}

bool WaveEmulationAudioProcessorEditor::handlePerformanceControl(
    juce::Point<float> designPoint)
{
    // Exact control geometry from WaldorfWaveUI_NOLOGO.svg. These interactions draw
    // nothing; the supplied SVG remains the only panel artwork.
    if (hitRectangle(designPoint, 1372.0f, 538.0f, 61.0f, 30.0f))
    {
        pressPanelButton(wave::panel::matrixIndexForDiagnosticCode(39));
        performanceMode = true;
        return true;
    }

    struct PerformanceButton
    {
        float x;
        float y;
        int diagnosticCode;
    };
    // These are physical serial inputs, not shortcuts into the embedded
    // factory bank. Sending every selector through OS 1.700 is essential
    // after Total Recall, when the Wave's active RAM bank came from disk.
    constexpr std::array<PerformanceButton, 15> performanceButtons {
        PerformanceButton { 1543.0f, 553.0f, 44 }, // Bank
        PerformanceButton { 1602.0f, 553.0f, 45 }, // 1__
        PerformanceButton { 1680.0f, 553.0f, 47 }, // Hold
        PerformanceButton { 1602.0f, 435.0f, 48 }, // 7
        PerformanceButton { 1641.0f, 435.0f, 51 }, // 8
        PerformanceButton { 1680.0f, 435.0f, 54 }, // 9
        PerformanceButton { 1602.0f, 475.0f, 53 }, // 4
        PerformanceButton { 1641.0f, 475.0f, 50 }, // 5
        PerformanceButton { 1680.0f, 475.0f, 49 }, // 6
        PerformanceButton { 1602.0f, 513.0f, 46 }, // 1
        PerformanceButton { 1641.0f, 513.0f, 56 }, // 2
        PerformanceButton { 1680.0f, 513.0f, 55 }, // 3
        PerformanceButton { 1641.0f, 553.0f, 52 }, // 0
        PerformanceButton { 1253.0f, 553.0f, 69 }, // Minus
        PerformanceButton { 1292.0f, 553.0f, 72 }  // Plus
    };
    for (const auto& button : performanceButtons)
    {
        if (hitCircle(designPoint, button.x, button.y))
        {
            pressPanelButton(
                wave::panel::matrixIndexForDiagnosticCode(button.diagnosticCode));
            return true;
        }
    }

    if (hitCircle(designPoint, 903.0f, 553.0f)
        || hitCircle(designPoint, 942.0f, 553.0f))
    {
        const auto diagnosticCode
            = hitCircle(designPoint, 903.0f, 553.0f) ? 21 : 23;
        pressPanelButton(
            wave::panel::matrixIndexForDiagnosticCode(diagnosticCode));
        return true;
    }

    return false;
}

void WaveEmulationAudioProcessorEditor::pressPanelButton(int hardwareId)
{
    if (hardwareId < 0)
        return;
    const auto diagnosticCode
        = wave::panel::diagnosticCodeForMatrixIndex(hardwareId);
    // Recall/Init is a firmware modal. The eight Instrument switches become
    // display soft keys while it is open, even if it was entered from the
    // Performance screen. OK/Cancel return them to the previously selected
    // blue operating mode.
    if (diagnosticCode == 40)
        performanceMode = false;
    else if (hardwareId == 71 || hardwareId == 70)
        performanceMode = selectedModeSwitch == 39;
    // Always restore the released edge before a new press. If the host took
    // mouse capture during the previous gesture, JUCE may not have delivered
    // mouseUp; without this edge, pressing the same physical switch again is
    // invisible to the firmware's button scanner.
    if (activePanelButton >= 0)
        ownerProcessor.setPanelButton(activePanelButton, false);
    activePanelButton = hardwareId;
    if (!ownerProcessor.setPanelButton(hardwareId, true))
    {
        activePanelButton = -1;
        repaint();
        return;
    }
    updateLocalButtonState(diagnosticCode);
    // The engine may reject a mode choice which the current firmware page
    // does not permit (notably Option and Wave Edit). Consume that feedback immediately so
    // neither contextual controls nor their LEDs transiently adopt the
    // ignored button's mode before the next timer tick.
    selectedModeSwitch = ownerProcessor.getPanelSelectedMode();
    performanceMode = selectedModeSwitch == 39;
    if (performanceMode)
        selectedEditSwitch = -1;
    // OS 1.700 does not enter an operation page for the reserved Sequencer
    // key, so retain the last firmware-backed working mode.
    constexpr std::array modeSwitches { 38, 33, 34, 32, 37, 36, 39 };
    if (std::find(modeSwitches.begin(), modeSwitches.end(), diagnosticCode)
        != modeSwitches.end())
        updateWaveEnvelopeKnobBindings();
    repaint();
}

void WaveEmulationAudioProcessorEditor::updateLocalButtonState(int hardwareId)
{
    if (hardwareId < 0)
        return;
    constexpr std::array modeSwitches { 38, 33, 34, 32, 37, 36, 39 };
    if (std::find(modeSwitches.begin(), modeSwitches.end(), hardwareId)
        != modeSwitches.end())
    {
        selectedModeSwitch = hardwareId;
        performanceMode = hardwareId == 39;
        if (performanceMode)
            selectedEditSwitch = -1;
    }

    if (performanceMode
        && std::find(instrumentSwitches.begin(), instrumentSwitches.end(), hardwareId)
            != instrumentSwitches.end()
        && ownerProcessor.getInstrumentButtonMode()
               == WaveEmulationAudioProcessor::InstrumentButtonMode::normal)
        selectedInstrumentSwitch = hardwareId;

    constexpr std::array editSwitches {
        8, 17, 86, 11, 60, 67, 9, 18, 59, 66, 10, 19, 61,
        62, 64, 65, 63
    };
    if (std::find(editSwitches.begin(), editSwitches.end(), hardwareId)
        != editSwitches.end())
        selectedEditSwitch = selectedEditSwitch == hardwareId ? -1 : hardwareId;

    if (hardwareId == 2)
        selectedLfo = 1 - selectedLfo;
    else if (hardwareId == 14)
        waveEnvelopePage = (waveEnvelopePage + 1) % 3;
    else if (hardwareId == 75)
        filterSelection = 1 - filterSelection;
}

juce::Colour WaveEmulationAudioProcessorEditor::panelLedColour(
    const PanelLed& led) const noexcept
{
    const auto green = led.greenSerialCode >= 0
                       && ownerProcessor.getPanelLed(led.greenSerialCode);
    const auto red = led.redSerialCode >= 0
                     && ownerProcessor.getPanelLed(led.redSerialCode);
    if (green && red)
        return juce::Colours::yellow;
    if (green)
        return juce::Colours::green;
    if (red)
        return juce::Colours::red;
    return juce::Colours::transparentBlack;
}

void WaveEmulationAudioProcessorEditor::mouseDrag(const juce::MouseEvent& event)
{
    if (midiKeyboardDragging)
    {
        const auto midiNote = midiNoteAt(viewToDesign(event.position));
        if (midiNote != activeMidiNote)
        {
            if (activeMidiNote >= 0)
                ownerProcessor.noteOffFromUi(activeMidiNote);
            activeMidiNote = midiNote;
            if (activeMidiNote >= 0)
                ownerProcessor.noteOnFromUi(activeMidiNote);
            repaint();
        }
        return;
    }

    if (activePerformanceWheel >= 0)
    {
        const auto wheel = static_cast<size_t>(activePerformanceWheel);
        const auto value = juce::jlimit(
            0.0f, 1.0f,
            analogDragStartValue
                + (analogDragStartY - event.position.y) / analogDragRangePixels);
        performanceWheelValues[wheel] = value;
        if (wheel == 0)
            ownerProcessor.setPitchWheelFromUi(value);
        else if (wheel == 1)
            ownerProcessor.setModWheelFromUi(value);
        else
            ownerProcessor.setFreeWheelFromUi(value);
        repaint();
        return;
    }

    if (activePanelAnalog < 0)
        return;
    const auto value = juce::jlimit(0.0f, 1.0f,
                                    analogDragStartValue
                                        + (analogDragStartY - event.position.y)
                                              / analogDragRangePixels);
    panelAnalogValues[static_cast<size_t>(activePanelAnalog)] = value;
    if (activePanelFader >= 0)
        ownerProcessor.setPanelFader(activePanelFader, activePanelAnalog, value,
                                     activePanelFaderPerformanceMode);
    else
        ownerProcessor.setPanelAnalog(activePanelAnalog, value);
    repaint();
}

void WaveEmulationAudioProcessorEditor::releaseActivePointerInteractions(
    bool showMidiFeedback)
{
    if (midiKeyboardDragging)
    {
        if (activeMidiNote >= 0)
        {
            ownerProcessor.noteOffFromUi(activeMidiNote);
            if (showMidiFeedback)
            {
                recentMidiNote = activeMidiNote;
                recentMidiNoteHighlightUntil
                    = juce::Time::getMillisecondCounterHiRes()
                      + recentMidiFeedbackMilliseconds;
            }
        }
        activeMidiNote = -1;
        midiKeyboardDragging = false;
        repaint();
    }
    if (activePanelButton >= 0)
    {
        ownerProcessor.setPanelButton(activePanelButton, false);
        activePanelButton = -1;
    }
    if (activeKeyboardButton >= 0)
    {
        ownerProcessor.setKeyboardControllerButton(
            static_cast<uint8_t>(activeKeyboardButton), false);
        activeKeyboardButton = -1;
    }
    if (activePanelFader >= 0)
    {
        ownerProcessor.endPanelFaderGesture(activePanelFader,
                                             activePanelFaderPerformanceMode);
        activePanelFader = -1;
        activePanelFaderPerformanceMode = false;
    }
    if (activePerformanceWheel >= 0)
    {
        // Pitch Bend is spring-centred. Both modulation wheels retain their
        // last physical positions; Free is bipolar around its centre marker.
        if (activePerformanceWheel == 0)
        {
            performanceWheelValues[0] = 0.5f;
            ownerProcessor.setPitchWheelFromUi(8192.0f / 16383.0f);
        }
        activePerformanceWheel = -1;
        repaint();
    }
    activePanelAnalog = -1;
}

void WaveEmulationAudioProcessorEditor::mouseUp(const juce::MouseEvent&)
{
    releaseActivePointerInteractions(true);
}

void WaveEmulationAudioProcessorEditor::mouseExit(const juce::MouseEvent& event)
{
    if (activeMidiNote >= 0)
    {
        ownerProcessor.noteOffFromUi(activeMidiNote);
        recentMidiNote = activeMidiNote;
        recentMidiNoteHighlightUntil = juce::Time::getMillisecondCounterHiRes()
                                       + recentMidiFeedbackMilliseconds;
        activeMidiNote = -1;
        repaint();
    }
    if (!event.mods.isAnyMouseButtonDown())
        mouseUp(event);
}

void WaveEmulationAudioProcessorEditor::initialisePanelRegions(const juce::XmlElement& svg)
{
    panelRegions.clear();
    for (auto* element : svg.getChildIterator())
    {
        juce::Path path;
        juce::Point<float> centre;
        auto isPanelControl = false;
        auto analog = false;
        auto fader = false;
        auto editButton = false;
        auto roundButton = false;
        auto verticalButton = false;

        if (element->hasTagName("circle"))
        {
            const auto radius = static_cast<float>(element->getDoubleAttribute("r"));
            if ((radius >= 14.0f && radius <= 17.0f)
                || std::abs(radius - 45.0f) < 0.1f)
            {
                centre = { static_cast<float>(element->getDoubleAttribute("cx")),
                           static_cast<float>(element->getDoubleAttribute("cy")) };
                path.addEllipse(centre.x - radius, centre.y - radius,
                                radius * 2.0f, radius * 2.0f);
                isPanelControl = true;
                analog = radius >= 16.0f;
                roundButton = std::abs(radius - 14.5f) < 0.1f
                              && element->getStringAttribute("fill") == "#676767";
            }
        }
        else if (element->hasTagName("rect"))
        {
            const auto width = static_cast<float>(element->getDoubleAttribute("width"));
            const auto height = static_cast<float>(element->getDoubleAttribute("height"));
            const auto radius = static_cast<float>(element->getDoubleAttribute("rx"));
            const auto isEditPill = std::abs(width - 61.0f) < 0.1f
                                    && std::abs(height - 30.0f) < 0.1f
                                    && std::abs(radius - 15.0f) < 0.1f;
            // Store is the one red 60 x 29 pill in the supplied SVG. Its
            // slightly different Figma geometry is intentional artwork, not
            // a reason to omit the physical switch from the panel scanner.
            const auto isStorePill = std::abs(width - 60.0f) < 0.1f
                                     && std::abs(height - 29.0f) < 0.1f
                                     && std::abs(radius - 14.5f) < 0.1f
                                     && element->getStringAttribute("fill") == "#DA2E2E";
            const auto isFader = std::abs(width - 28.0f) < 0.1f
                                 && std::abs(height - 169.0f) < 0.1f;
            if (isEditPill || isStorePill || isFader)
            {
                const auto x = static_cast<float>(element->getDoubleAttribute("x"));
                const auto y = static_cast<float>(element->getDoubleAttribute("y"));
                centre = { x + width * 0.5f, y + height * 0.5f };
                path.addRoundedRectangle(x, y, width, height, radius);

                auto transformText = element->getStringAttribute("transform");
                if (transformText.startsWith("rotate(") && transformText.endsWithChar(')'))
                {
                    transformText = transformText.substring(7, transformText.length() - 1);
                    auto values = juce::StringArray::fromTokens(transformText, " ,", "");
                    if (values.size() >= 3)
                    {
                        const auto angle = static_cast<float>(values[0].getDoubleValue());
                        const auto pivotX = static_cast<float>(values[1].getDoubleValue());
                        const auto pivotY = static_cast<float>(values[2].getDoubleValue());
                        const auto transform = juce::AffineTransform::rotation(
                            juce::degreesToRadians(angle), pivotX, pivotY);
                        path.applyTransform(transform);
                        centre = centre.transformedBy(transform);
                        editButton = isEditPill && std::abs(angle + 45.0f) < 0.1f;
                        verticalButton = isEditPill && std::abs(angle + 90.0f) < 0.1f;
                    }
                }
                isPanelControl = true;
                analog = isFader;
                fader = isFader;
            }
        }

        if (isPanelControl)
            panelRegions.push_back(
                { std::move(path), centre, -1, -1, analog, fader, editButton, roundButton,
                  verticalButton, -1, 0.0f, 0.0f });
    }

    // The 6 x 6 magenta circles in the Figma source are LED placement
    // markers. Tick marks use strokes and are deliberately excluded here.
    panelLeds.clear();
    for (auto* element : svg.getChildIterator())
    {
        if (!element->hasTagName("circle")
            || std::abs(element->getDoubleAttribute("r") - 3.0) >= 0.1
            || element->getStringAttribute("fill") != "#6C0455")
            continue;
        const auto centre = juce::Point<float> {
            static_cast<float>(element->getDoubleAttribute("cx")),
            static_cast<float>(element->getDoubleAttribute("cy"))
        };
        if (std::none_of(panelLeds.begin(), panelLeds.end(), [centre](const auto& led) {
                return led.centre.getDistanceFrom(centre) < 0.1f;
            }))
            panelLeds.push_back({ centre });
    }

    // Match each fader recess to the dark 6 x 123 track drawn inside it in the
    // supplied Figma SVG. The handle centre follows that line exactly.
    for (auto* element : svg.getChildIterator())
    {
        if (!element->hasTagName("rect"))
            continue;
        const auto width = static_cast<float>(element->getDoubleAttribute("width"));
        const auto height = static_cast<float>(element->getDoubleAttribute("height"));
        if (std::abs(width - 6.0f) >= 0.1f || std::abs(height - 123.0f) >= 0.1f)
            continue;
        const auto x = static_cast<float>(element->getDoubleAttribute("x"));
        const auto y = static_cast<float>(element->getDoubleAttribute("y"));
        const auto trackCentreX = x + width * 0.5f;
        auto* nearestFader = static_cast<PanelRegion*>(nullptr);
        auto nearestDistance = std::numeric_limits<float>::max();
        for (auto& region : panelRegions)
        {
            if (!region.fader)
                continue;
            const auto distance = std::abs(region.centre.x - trackCentreX);
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestFader = &region;
            }
        }
        if (nearestFader != nullptr && nearestDistance < 1.0f)
        {
            nearestFader->faderTrackTop = y;
            nearestFader->faderTrackBottom = y + height;
        }
    }

    // Bind artwork coordinates only through the single OS/service-manual
    // wiring contract. An unmatched control remains inert instead of
    // inheriting a coincidental scan-order ID belonging to another switch.
    for (auto& region : panelRegions)
    {
        region.hardwareId = -1;
        if (region.analog)
            continue;
        if (const auto* wiring = wave::panel::switchAt(
                region.centre.x - panelHorizontalOffset, region.centre.y))
            region.hardwareId = wave::panel::physicalMatrixIndex(*wiring);
        else if (const auto* keyboardPanelWiring = wave::panel::keyboardPanelSwitchAt(
                     region.centre.x, region.centre.y))
            region.hardwareId = wave::panel::physicalMatrixIndex(*keyboardPanelWiring);
        else if (const auto* keyboard = wave::panel::keyboardControllerButtonAt(
                     region.centre.x, region.centre.y))
            region.keyboardCode = keyboard->asciiCode;
    }

    // LED diagnostic serial codes are already the physical 8 x 16 output
    // matrix indices. The bi-colour Instrument LEDs have independent red and
    // green serial outputs at the same exact Figma marker.
    for (auto& led : panelLeds)
        if (const auto* wiring = wave::panel::ledAt(
                led.centre.x - panelHorizontalOffset, led.centre.y))
        {
            led.redSerialCode = wiring->redSerialCode;
            led.greenSerialCode = wiring->greenSerialCode;
        }
        else if (const auto* keyboardPanelWiring = wave::panel::keyboardPanelLedAt(
                     led.centre.x, led.centre.y))
        {
            led.redSerialCode = keyboardPanelWiring->redSerialCode;
            led.greenSerialCode = keyboardPanelWiring->greenSerialCode;
        }

    editButtonDesignCentres.clear();
    editButtonHardwareIds.clear();
    for (const auto& region : panelRegions)
    {
        if (!region.editButton)
            continue;
        editButtonDesignCentres.push_back(region.centre);
        editButtonHardwareIds.push_back(region.hardwareId);
    }

    std::array<PanelRegion*, 8> faders {};
    auto faderCount = 0;
    for (auto& region : panelRegions)
        if (region.fader && faderCount < static_cast<int>(faders.size()))
            faders[static_cast<size_t>(faderCount++)] = &region;
    std::sort(faders.begin(), faders.begin() + faderCount,
              [](const auto* left, const auto* right) {
                  return left->centre.x < right->centre.x;
              });
    // The panel multiplexer does not number the eight display faders in
    // geometric order. These are the genuine OS 1.700 ADC selections, mapped
    // left-to-right by sweeping the running firmware and confirming the LCD
    // field changed by each input.
    for (auto index = 0; index < faderCount; ++index)
    {
        auto* fader = faders[static_cast<size_t>(index)];
        fader->faderIndex = index;
        fader->hardwareId
            = wave::panel::performanceFaderAdcChannels[static_cast<size_t>(index)];
    }
}

void WaveEmulationAudioProcessorEditor::synchroniseFaderValues() noexcept
{
    for (const auto& region : panelRegions)
        if (region.fader && region.faderIndex >= 0 && region.hardwareId >= 0)
            panelAnalogValues[static_cast<size_t>(region.hardwareId)]
                = ownerProcessor.getPanelFaderValue(region.faderIndex);
}

void WaveEmulationAudioProcessorEditor::timerCallback()
{
    if (recentMidiNote >= 0
        && juce::Time::getMillisecondCounterHiRes() >= recentMidiNoteHighlightUntil)
        recentMidiNote = -1;
    if (activePerformanceWheel < 0)
    {
        performanceWheelValues[0] = ownerProcessor.getPitchWheelForUi();
        performanceWheelValues[1] = ownerProcessor.getModWheelForUi();
        performanceWheelValues[2] = ownerProcessor.getFreeWheelForUi();
    }
    for (const auto& knob : knobs)
        if (const auto physical
            = ownerProcessor.getPanelPotValue(knob->parameterId()))
            knob->synchronisePhysicalPosition(*physical);
    for (size_t oscillator = 0; oscillator < oscillatorOctavePositions.size(); ++oscillator)
        oscillatorOctavePositions[oscillator]
            = 2 - ownerProcessor.getFirmwareOscillatorOctave(
                      static_cast<int>(oscillator));

    // Mode indication and the contextual panel controls follow the engine's
    // serial-panel state. In particular, firmware CANCEL from Disk returns to
    // Performance without the editor inventing a second panel action.
    const auto engineMode = ownerProcessor.getPanelSelectedMode();
    if (selectedModeSwitch != engineMode)
    {
        selectedModeSwitch = engineMode;
        performanceMode = engineMode == 39;
        if (performanceMode)
            selectedEditSwitch = -1;
        updateWaveEnvelopeKnobBindings();
    }
    if (performanceMode && selectedEditSwitch < 0 && activePanelFader < 0)
        synchroniseFaderValues();
    const auto selectedInstrument = ownerProcessor.getSelectedPerformanceInstrument();
    if (selectedInstrument >= 0
        && selectedInstrument < static_cast<int>(instrumentSwitches.size()))
        selectedInstrumentSwitch
            = instrumentSwitches[static_cast<size_t>(selectedInstrument)];
    if (hasKeyboardFocus(true))
    {
        updateComputerKeyboardNotes();
        updateComputerKeyboardShift();
    }
    else
    {
        releaseComputerKeyboardNotes();
        releaseComputerKeyboardShift();
    }
    repaint();
}

void WaveEmulationAudioProcessorEditor::resized()
{
    rebuildPanelImage();
    layoutComponents();
}

void WaveEmulationAudioProcessorEditor::layoutComponents()
{
    const auto place = [this](juce::Component& component, juce::Rectangle<float> designBounds) {
        const auto bounds = designToView(designBounds);
        component.setVisible(bounds.has_value());
        if (bounds.has_value())
            component.setBounds(*bounds);
    };

    if (systemMenuButton != nullptr)
    {
        // Classic: the narrow chassis rail, free of panel controls. Tabbed:
        // left of the tab buttons, on every tab.
        const auto bounds = tabbedLayout
                                ? juce::Rectangle<float> { 10.0f, 7.0f, 36.0f, 28.0f }
                                : juce::Rectangle<float> { 16.0f, 4.0f, 36.0f, 28.0f };
        systemMenuButton->iconColour = tabbedLayout ? tabHeaderColour
                                                    : juce::Colour(0xffdce4ef);
        systemMenuButton->setBounds((bounds * viewScale()).toNearestInt());
        systemMenuButton->toFront(false);
    }

    if (lcd != nullptr)
        place(*lcd, { 873.0f + panelHorizontalOffset, 237.0f, 448.0f, 70.0f });

    if (knobDesignCentres.size() == knobs.size()
        && knobArtworkTypes.size() == knobs.size())
    {
        for (size_t index = 0; index < knobs.size(); ++index)
        {
            const auto hitSize = knobArtworkTypes[index] == KnobArtworkType::largeRed
                                     ? 104.0f : 60.0f;
            place(*knobs[index],
                  juce::Rectangle<float>(hitSize, hitSize)
                      .withCentre(knobDesignCentres[index].translated(
                          panelHorizontalOffset, 0.0f)));
        }
    }

    if (editButtonDesignCentres.size() == editButtons.size())
    {
        for (size_t index = 0; index < editButtons.size(); ++index)
        {
            place(*editButtons[index],
                  juce::Rectangle<float>(65.0f, 65.0f)
                      .withCentre(editButtonDesignCentres[index]));
            // Parameter knobs are constructed later and would otherwise sit
            // above the small overlapping edge of a diagonal Edit switch.
            // Only the exact pill path accepts a click, so its transparent
            // corners still pass through to the neighbouring control.
            editButtons[index]->toFront(false);
        }
    }
}

void WaveEmulationAudioProcessorEditor::rebuildPanelImage()
{
    if (panelArtwork == nullptr || getWidth() <= 0 || getHeight() <= 0)
    {
        panelImage = {};
        return;
    }

    // The supplied Figma SVG remains the source of truth. Rasterising it once
    // at the current editor size avoids retracing thousands of vector paths on
    // every LED, fader, or LCD refresh.
    constexpr auto artworkScale = 2;
    const auto fullArtworkWidth = juce::roundToInt(designWidth * viewScale());
    const auto fullArtworkHeight = juce::roundToInt(designHeight * viewScale());
    panelImage = juce::Image(juce::Image::RGB, fullArtworkWidth * artworkScale,
                             fullArtworkHeight * artworkScale, true);
    juce::Graphics imageGraphics(panelImage);
    imageGraphics.fillAll(juce::Colours::black);
    panelArtwork->drawWithin(imageGraphics, panelImage.getBounds().toFloat(),
                             juce::RectanglePlacement::stretchToFit, 1.0f);
}

WaveEmulationAudioProcessorEditor::ParameterKnob&
WaveEmulationAudioProcessorEditor::addKnob(const juce::String& parameterId,
                                            const juce::String& displayName,
                                            juce::Point<float> designCentre,
                                            KnobArtworkType artwork,
                                            bool endlessRelative)
{
    auto knob = std::make_unique<ParameterKnob>(ownerProcessor.parameters, parameterId,
                                                displayName, false, endlessRelative);
    auto& result = *knob;
    if (!endlessRelative)
    {
        result.setPhysicalPotCallback(
            [this](const juce::String& boundParameterId, float normalised) {
                ownerProcessor.setPanelPotValue(boundParameterId, normalised);
            });
        if (const auto physical = ownerProcessor.getPanelPotValue(parameterId))
            result.synchronisePhysicalPosition(*physical);
    }
    addAndMakeVisible(result);
    knobs.push_back(std::move(knob));
    knobDesignCentres.push_back(designCentre);
    knobArtworkTypes.push_back(artwork);
    return result;
}

void WaveEmulationAudioProcessorEditor::updateLfoKnobBindings()
{
    if (std::any_of(lfoKnobs.begin(), lfoKnobs.end(), [](const auto* knob) {
            return knob == nullptr;
        }))
        return;

    const auto lfo = static_cast<size_t>(juce::jlimit(0, 1, selectedLfo));
    const auto rateRoute = lfo == 0 ? wave::parameters::lfo1RateMod
                                    : wave::parameters::lfo2RateMod;
    const auto levelRoute = lfo == 0 ? wave::parameters::lfo1LevelMod
                                     : wave::parameters::lfo2LevelMod;
    const auto number = juce::String(lfo + 1);
    lfoKnobs[0]->setBinding(wave::parameters::lfoRate[lfo], "LFO " + number + " Ratio");
    lfoKnobs[1]->setBinding(wave::parameters::modulationAmount[rateRoute],
                            "LFO " + number + " Rate Mod Amount");
    lfoKnobs[2]->setBinding(wave::parameters::modulationAmount[levelRoute],
                            "LFO " + number + " Level Mod Amount");
}

void WaveEmulationAudioProcessorEditor::updateWaveEnvelopeKnobBindings()
{
    for (size_t knob = 0; knob < waveEnvelopeTimeKnobs.size(); ++knob)
    {
        if (waveEnvelopeTimeKnobs[knob] == nullptr || waveEnvelopeLevelKnobs[knob] == nullptr)
            return;
    }

    if (selectedModeSwitch == 38)
    {
        constexpr std::array<const char*, 8> names {
            "Quick Attack", "Quick Decay", "Quick Sustain", "Quick Release",
            "Quick Pitch Mod", "Quick Timbre", "Quick Timbre Mod", "Quick Wavescan"
        };
        for (size_t knob = 0; knob < 4; ++knob)
        {
            waveEnvelopeTimeKnobs[knob]->setBinding(
                wave::parameters::quickEdit[knob], names[knob]);
            waveEnvelopeLevelKnobs[knob]->setBinding(
                wave::parameters::quickEdit[knob + 4], names[knob + 4]);
        }
        return;
    }

    if (waveEnvelopePage == 2)
    {
        for (size_t knob = 0; knob < 4; ++knob)
        {
            const auto number = juce::String(knob + 1);
            waveEnvelopeTimeKnobs[knob]->setBinding(
                wave::parameters::freeEnvelopeTime[knob],
                "Free Envelope Time " + number);
            waveEnvelopeLevelKnobs[knob]->setBinding(
                wave::parameters::freeEnvelopeLevel[knob],
                "Free Envelope Level " + number);
        }
        return;
    }

    const auto firstPoint = static_cast<size_t>(juce::jlimit(0, 1, waveEnvelopePage) * 4);
    for (size_t knob = 0; knob < 4; ++knob)
    {
        const auto point = firstPoint + knob;
        const auto number = juce::String(point + 1);
        waveEnvelopeTimeKnobs[knob]->setBinding(
            wave::parameters::waveEnvelopeTime[point], "Wave Envelope Time " + number);
        waveEnvelopeLevelKnobs[knob]->setBinding(
            wave::parameters::waveEnvelopeLevel[point], "Wave Envelope Level " + number);
    }
}

juce::String WaveEmulationAudioProcessorEditor::getTooltip()
{
    return tooltipAt(getMouseXYRelative().toFloat());
}

juce::String WaveEmulationAudioProcessorEditor::tooltipAt(juce::Point<float> point) const
{
    if (getWidth() <= 0)
        return {};
    for (const auto& knob : knobs)
        if (knob->isVisible() && knob->getBounds().toFloat().contains(point))
            return knob->getTooltip();
    const auto designPoint = viewToDesign(point);
    for (const auto& region : panelRegions)
        if (region.fader && region.faderIndex >= 0
            && region.path.contains(designPoint.x, designPoint.y))
            return "Fader " + juce::String(region.faderIndex + 1)
                   + (performanceMode && selectedEditSwitch < 0
                          ? " - assigned performance control"
                          : " - parameter shown above on the LCD")
                   + "\nDrag up/down to adjust.";
    return {};
}

juce::Result WaveEmulationAudioProcessorEditor::loadPanelSkin(const juce::File& file,
                                                             bool remember)
{
    const auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr || !xml->hasTagName("svg"))
        return juce::Result::fail("Choose a valid SVG panel skin.");
    auto viewBox = juce::StringArray::fromTokens(xml->getStringAttribute("viewBox"), " ,\t\r\n", "");
    viewBox.removeEmptyStrings();
    if (viewBox.size() != 4 || viewBox[0].getDoubleValue() != 0.0
        || viewBox[1].getDoubleValue() != 0.0
        || !juce::approximatelyEqual(viewBox[2].getDoubleValue(), static_cast<double>(designWidth))
        || !juce::approximatelyEqual(viewBox[3].getDoubleValue(), static_cast<double>(designHeight))
        || !juce::approximatelyEqual(xml->getDoubleAttribute("width"), static_cast<double>(designWidth))
        || !juce::approximatelyEqual(xml->getDoubleAttribute("height"), static_cast<double>(designHeight)))
        return juce::Result::fail("Panel skins must use width=2338, height=1042 and viewBox=\"0 0 2338 1042\". Keep controls in their original positions.");
    auto artwork = juce::Drawable::createFromSVG(*xml);
    if (artwork == nullptr)
        return juce::Result::fail("The SVG panel skin could not be rendered.");
    if (remember)
    {
        const auto result = ownerProcessor.rememberPanelSkin(file);
        if (result.failed())
            return result;
    }
    panelArtwork = std::move(artwork);
    panelSkinFile = file;
    // Interaction geometry always comes from the original panel, so changing
    // label paths/groups cannot change firmware wiring or fader destinations.
    rebuildPanelImage();
    repaint();
    return juce::Result::ok();
}

void WaveEmulationAudioProcessorEditor::useDefaultPanelSkin()
{
    const auto xml = juce::XmlDocument::parse(juce::String::fromUTF8(
        WaveAssets::WaldorfWaveUI_NOLOGO_svg, WaveAssets::WaldorfWaveUI_NOLOGO_svgSize));
    if (xml == nullptr)
        return;
    const auto result = ownerProcessor.rememberPanelSkin({});
    if (result.failed())
    {
        showDiskError("Panel Skin", result);
        return;
    }
    panelArtwork = juce::Drawable::createFromSVG(*xml);
    panelSkinFile = juce::File{};
    rebuildPanelImage();
    repaint();
}

void WaveEmulationAudioProcessorEditor::showPanelSkinChooser()
{
    panelSkinChooser = std::make_unique<juce::FileChooser>(
        "Load Alternative Panel Skin", panelSkinFile, "*.svg");
    panelSkinChooser->launchAsync(juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer(this)](const juce::FileChooser& chooser) {
            if (safe == nullptr || chooser.getResult() == juce::File{})
                return;
            const auto result = safe->loadPanelSkin(chooser.getResult());
            if (result.failed())
                safe->showDiskError("Panel Skin", result);
        });
}
