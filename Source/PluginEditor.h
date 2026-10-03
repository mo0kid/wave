#pragma once

#include "PluginProcessor.h"
#include "UI/WaveLcdComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>
#include <optional>
#include <vector>

class WaveEmulationAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                private juce::Timer,
                                                private juce::MenuBarModel,
                                                public juce::TooltipClient
{
public:
    explicit WaveEmulationAudioProcessorEditor(WaveEmulationAudioProcessor&);
    ~WaveEmulationAudioProcessorEditor() override;

    juce::String getTooltip() override;
    [[nodiscard]] juce::String tooltipAt(juce::Point<float> editorPoint) const;
    juce::Result loadPanelSkin(const juce::File& file, bool remember = true);
    void useDefaultPanelSkin();
    void setTabbedLayout(bool tabbed, bool remember = true);
    void selectTab(int tab);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isKeyDown) override;
    void focusLost(FocusChangeType) override;

private:
    class WaveLookAndFeel;
    class ParameterKnob;
    class ArtworkButton;
    class SystemMenuButton;

    enum class KnobArtworkType
    {
        standard,
        red,
        largeRed
    };

    struct PanelRegion
    {
        juce::Path path;
        juce::Point<float> centre;
        int hardwareId = -1;
        int keyboardCode = -1;
        bool analog = false;
        bool fader = false;
        bool editButton = false;
        bool roundButton = false;
        bool verticalButton = false;
        int faderIndex = -1;
        float faderTrackTop = 0.0f;
        float faderTrackBottom = 0.0f;
    };

    struct PanelLed
    {
        juce::Point<float> centre;
        int redSerialCode = -1;
        int greenSerialCode = -1;
    };

    ParameterKnob& addKnob(const juce::String& parameterId, const juce::String& displayName,
                           juce::Point<float> designCentre,
                           KnobArtworkType artwork = KnobArtworkType::standard,
                           bool endlessRelative = false);
    void updateLfoKnobBindings();
    void updateWaveEnvelopeKnobBindings();
    void initialisePanelRegions(const juce::XmlElement& svg);
    void synchroniseFaderValues() noexcept;
    void rebuildPanelImage();
    void layoutComponents();
    void paintDesign(juce::Graphics&, float scale);
    [[nodiscard]] float viewScale() const noexcept;
    [[nodiscard]] juce::Point<float> viewSize() const noexcept;
    [[nodiscard]] juce::Point<float> viewToDesign(juce::Point<float> viewPoint) const noexcept;
    [[nodiscard]] std::optional<juce::Rectangle<int>> designToView(
        juce::Rectangle<float> designBounds) const noexcept;
    bool handlePerformanceControl(juce::Point<float> designPoint);
    void pressPanelButton(int hardwareId);
    void releaseActivePointerInteractions(bool showMidiFeedback);
    void updateLocalButtonState(int hardwareId);
    [[nodiscard]] juce::Colour panelLedColour(const PanelLed& led) const noexcept;
    bool updateComputerKeyboardNotes();
    bool updateComputerKeyboardShift();
    void releaseComputerKeyboardNotes();
    void releaseComputerKeyboardShift();
    void timerCallback() override;
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex,
                                    const juce::String& menuName) override;
    void menuItemSelected(int menuItemId, int topLevelMenuIndex) override;
    void showSystemMenu();
    void showFirmwareFolderChooser();
    void showPanelSkinChooser();
    void showDiskImageChooser();
    void showCreateBlankDiskChooser();
    void showCreateDiskFromSetChooser();
    void showCreateDiskFromWavetableChooser();
    void showDiskDestinationChooser(const juce::File& waveSetup);
    void showSaveDiskAsChooser();
    void showDiskError(const juce::String& title, const juce::Result& result);
    void setWindowScale(float scale);
    void setKeyboardVisible(bool visible);

    WaveEmulationAudioProcessor& ownerProcessor;
    std::unique_ptr<WaveLookAndFeel> lookAndFeel;
    std::vector<std::unique_ptr<ParameterKnob>> knobs;
    std::unique_ptr<wave::ui::WaveLcdComponent> lcd;
    std::unique_ptr<juce::Drawable> panelArtwork;
    juce::Image panelImage;
    juce::File panelSkinFile;
    juce::TooltipWindow tooltipWindow { this, 650 };
    std::unique_ptr<juce::FileChooser> panelSkinChooser;
    std::unique_ptr<juce::Drawable> sliderArtwork;
    std::unique_ptr<juce::Drawable> knobArtwork;
    std::unique_ptr<juce::Drawable> redKnobArtwork;
    std::unique_ptr<juce::Drawable> largeRedKnobArtwork;
    std::unique_ptr<juce::Drawable> editButtonArtwork;
    std::unique_ptr<juce::Drawable> roundButtonArtwork;
    std::unique_ptr<juce::Drawable> verticalButtonArtwork;
    std::unique_ptr<juce::Drawable> ledOffArtwork;
    std::unique_ptr<juce::Drawable> ledOnGreenArtwork;
    std::unique_ptr<juce::Drawable> ledOnRedArtwork;
    std::unique_ptr<juce::Drawable> ledOnYellowArtwork;
    std::unique_ptr<juce::FileChooser> firmwareFolderChooser;
    std::unique_ptr<juce::FileChooser> diskImageChooser;
    std::unique_ptr<juce::FileChooser> waveSetChooser;
    std::unique_ptr<juce::FileChooser> diskDestinationChooser;
    std::unique_ptr<SystemMenuButton> systemMenuButton;
    std::vector<juce::Point<float>> knobDesignCentres;
    std::vector<KnobArtworkType> knobArtworkTypes;
    std::vector<std::unique_ptr<ArtworkButton>> editButtons;
    std::vector<juce::Point<float>> editButtonDesignCentres;
    std::vector<int> editButtonHardwareIds;
    std::vector<PanelLed> panelLeds;
    std::array<ParameterKnob*, 3> lfoKnobs{};
    std::array<ParameterKnob*, 4> waveEnvelopeTimeKnobs{};
    std::array<ParameterKnob*, 4> waveEnvelopeLevelKnobs{};
    std::vector<PanelRegion> panelRegions;
    int activePanelButton = -1;
    int activeKeyboardButton = -1;
    int activePanelAnalog = -1;
    int activePanelFader = -1;
    int activePerformanceWheel = -1;
    bool activePanelFaderPerformanceMode = false;
    int activeMidiNote = -1;
    int recentMidiNote = -1;
    double recentMidiNoteHighlightUntil = 0.0;
    bool midiKeyboardDragging = false;
    float analogDragStartY = 0.0f;
    float analogDragStartValue = 0.5f;
    float analogDragRangePixels = 160.0f;
    std::array<float, 128> panelAnalogValues{};
    std::array<float, 3> performanceWheelValues { 0.5f, 0.0f, 0.5f };
    std::array<bool, 13> keyboardNotes{};
    bool computerShiftDown = false;
    bool keyboardVisible = true;
    bool tabbedLayout = false;
    int selectedTab = 0;
    bool performanceMode = true;
    int selectedLfo = 0;
    int waveEnvelopePage = 0;
    int selectedModeSwitch = 39;
    int selectedInstrumentSwitch = 22;
    int selectedEditSwitch = -1;
    int filterSelection = 0;
    std::array<int, 2> oscillatorOctavePositions { 2, 2 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveEmulationAudioProcessorEditor)
};
