#include "PluginEditor.h"
#include "PanelWiring.h"
#include "UI/WaveLcdComponent.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class RepaintProbe final : public juce::Component
{
public:
    void paint(juce::Graphics& graphics) override
    {
        ++paintCount;
        graphics.fillAll(juce::Colours::red);
    }
    int paintCount = 0;
};

void snapshot(juce::Component& editor, const juce::String& name)
{
    auto image = editor.createComponentSnapshot(editor.getLocalBounds());
    require(image.isValid(), "Editor snapshot is invalid");
    // Catch missing artwork or a blank renderer without depending on platform
    // font rasterisation or exact pixel equality.
    const auto first = image.getPixelAt(0, 0);
    int different = 0;
    for (int y = 0; y < image.getHeight(); y += 16)
        for (int x = 0; x < image.getWidth(); x += 16)
            if (image.getPixelAt(x, y) != first)
                ++different;
    require(different > 100, "Editor snapshot is blank");
    if (const auto* directory = std::getenv("WAVE_TEST_ARTIFACT_DIR"))
    {
        juce::File folder(directory);
        require(folder.createDirectory().wasOk(), "Cannot create snapshot folder");
        auto stream = folder.getChildFile(name + ".png").createOutputStream();
        require(stream != nullptr && juce::PNGImageFormat{}.writeImageToStream(image, *stream),
                "Cannot write editor snapshot");
    }
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    try
    {
        juce::TemporaryFile preference(".txt");
        auto processor = std::make_unique<WaveEmulationAudioProcessor>(preference.getFile());
        processor->prepareToPlay(48000.0, 128);
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor->createEditor());
        require(editor != nullptr, "Plugin did not create an editor");
        editor->setVisible(true);
        require(editor->getWidth() == 2338 && editor->getHeight() == 1042,
                "Unexpected initial editor size");
        snapshot(*editor, "editor-full");
        auto* panelEditor = dynamic_cast<WaveEmulationAudioProcessorEditor*>(editor.get());
        require(panelEditor != nullptr, "Unexpected editor type");
        require(panelEditor->tooltipAt({ 222.0f, 81.0f }).contains("Wave 1 Detune"),
                "Knob tooltip does not identify its control");
        for (const auto x : { 969.0f, 1025.0f, 1080.0f, 1136.0f,
                              1191.0f, 1248.0f, 1302.0f, 1358.0f })
            require(panelEditor->tooltipAt({ x, 400.0f }).startsWith("Fader "),
                    "Missing fader tooltip");
        require(panelEditor->tooltipAt({ 10.0f, 600.0f }).isEmpty(),
                "Empty panel space should not have a control tooltip");

        // Tabbed layout: sections are translated beside a fixed screen block.
        panelEditor->setTabbedLayout(true, false);
        require(editor->getWidth() == 1446 && editor->getHeight() == 659,
                "Unexpected tabbed editor size");
        snapshot(*editor, "editor-tab-oscillator");
        require(panelEditor->tooltipAt({ 156.0f, 121.0f }).contains("Wave 1 Detune"),
                "Oscillator tab knob is not mapped");
        require(panelEditor->tooltipAt({ 969.0f - 66.0f, 440.0f }).startsWith("Fader "),
                "Screen faders are not mapped");
        panelEditor->selectTab(1);
        snapshot(*editor, "editor-tab-filter");
        require(panelEditor->tooltipAt({ 227.0f, 277.0f }).contains("Cutoff"),
                "Filter tab knob is not mapped");
        require(panelEditor->tooltipAt({ 156.0f, 121.0f }).isEmpty(),
                "Hidden oscillator knob still answers on the filter tab");
        panelEditor->selectTab(2);
        snapshot(*editor, "editor-tab-control");
        panelEditor->selectTab(0);
        panelEditor->setTabbedLayout(false, false);
        require(editor->getWidth() == 2338 && editor->getHeight() == 1042,
                "Classic layout did not restore its size");

        // A synthetic skin verifies visible replacement and fixed hit geometry.
        juce::TemporaryFile skin(".svg");
        require(skin.getFile().replaceWithText(
                    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2338\" height=\"1042\" "
                    "viewBox=\"0 0 2338 1042\"><rect width=\"2338\" height=\"1042\" fill=\"#00ff00\"/></svg>"),
                "Cannot write synthetic skin");
        require(panelEditor->loadPanelSkin(skin.getFile()).wasOk(), "Valid skin rejected");
        const auto skinImage = editor->createComponentSnapshot(editor->getLocalBounds());
        require(skinImage.getPixelAt(80, 20) == juce::Colours::lime,
                "Alternative skin did not replace panel artwork");
        require(panelEditor->tooltipAt({ 222.0f, 81.0f }).contains("Wave 1 Detune"),
                "Skin replaced control geometry");
        {
            std::unique_ptr<juce::AudioProcessorEditor> reopened(processor->createEditor());
            require(reopened->createComponentSnapshot(reopened->getLocalBounds())
                        .getPixelAt(80, 20) == juce::Colours::lime,
                    "Reopened editor did not remember the skin");
        }
        juce::TemporaryFile invalidSkin(".svg");
        invalidSkin.getFile().replaceWithText(
            "<svg width=\"100\" height=\"100\" viewBox=\"0 0 100 100\"/>");
        require(panelEditor->loadPanelSkin(invalidSkin.getFile()).failed(),
                "Skin with incompatible coordinates accepted");
        require(editor->createComponentSnapshot(editor->getLocalBounds())
                    .getPixelAt(80, 20) == juce::Colours::lime,
                "Rejected skin replaced working artwork");
        panelEditor->useDefaultPanelSkin();
        require(processor->getRememberedPanelSkin() == juce::File{},
                "Original skin did not clear the saved preference");
        preference.getFile().getSiblingFile(
            preference.getFile().getFileNameWithoutExtension() + "-panel-skin.txt").deleteFile();
        // A physical pot must move its visible artwork, even before firmware
        // or a factory SET is supplied. Exercise the actual child input handler.
        const juce::Point<float> knobCentre { 222.0f, 81.0f };
        auto* knob = editor->getComponentAt(knobCentre.roundToInt());
        require(knob != nullptr && knob != editor.get(), "Missing detune knob input component");
        const auto knobBounds = knob->getBounds();
        const auto beforeDrag = editor->createComponentSnapshot(knobBounds);
        const auto localStart = knob->getLocalPoint(editor.get(), knobCentre);
        const auto source = juce::Desktop::getInstance().getMainMouseSource();
        const auto time = juce::Time::getCurrentTime();
        const juce::MouseEvent down {
            source, localStart, juce::ModifierKeys { juce::ModifierKeys::leftButtonModifier },
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, knob, knob,
            time, localStart, time, 1, false
        };
        const juce::MouseEvent drag {
            source, localStart.translated(0.0f, 60.0f),
            juce::ModifierKeys { juce::ModifierKeys::leftButtonModifier },
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, knob, knob,
            time, localStart, time, 1, true
        };
        knob->mouseDown(down);
        knob->mouseDrag(drag);
        knob->mouseUp(drag);
        const auto afterDrag = editor->createComponentSnapshot(knobBounds);
        int changedPixels = 0;
        for (int y = 0; y < beforeDrag.getHeight(); ++y)
            for (int x = 0; x < beforeDrag.getWidth(); ++x)
                changedPixels += beforeDrag.getPixelAt(x, y) != afterDrag.getPixelAt(x, y) ? 1 : 0;
        require(changedPixels > 10, "Dragging a synth knob did not rotate its artwork");
        std::cout << "Synth knob drag changed " << changedPixels << " pixels" << std::endl;

        const auto command = [&](int key) {
            return editor->keyPressed(juce::KeyPress(
                key, juce::ModifierKeys { juce::ModifierKeys::commandModifier }, 0));
        };
        require(command('k') && editor->getWidth() == 2338 && editor->getHeight() == 619,
                "Hide Keyboard failed");
        snapshot(*editor, "editor-panel");
        require(command('=') && editor->getWidth() == 2572 && editor->getHeight() == 681,
                "Zoom In failed");
        require(command('-') && editor->getWidth() == 2338 && editor->getHeight() == 619,
                "Zoom Out failed");
        require(command('k') && command('0')
                    && editor->getWidth() == 2338 && editor->getHeight() == 1042,
                "Restore full editor failed");
        // Exercise editor destruction/recreation as hosts do when reopening UI.
        editor.reset();
        editor.reset(processor->createEditor());
        require(editor != nullptr, "Plugin did not reopen its editor");
        editor->setVisible(true);
        const auto loadAndCheckFirmware = [&] {
            if (const auto* directory = std::getenv("WAVE_FIRMWARE_DIR"))
            {
                require(processor->loadFirmware(juce::File(directory)).hasBothImages(),
                        "Cannot load LCD test firmware");
                juce::AudioBuffer<float> audio(2, 128);
                juce::MidiBuffer midi;
                for (int block = 0; block < 1000; ++block)
                {
                    audio.clear();
                    processor->processBlock(audio, midi);
                }
                // Public builds must accept panel input after user-supplied firmware
                // boots, without relying on an embedded factory sound bank.
                auto* panel = dynamic_cast<WaveEmulationAudioProcessorEditor*>(editor.get());
                require(panel != nullptr, "Unexpected editor type");
                for (const auto y : { 494.0f, 553.0f })
                {
                    const auto before = processor->getMasterFirmwareRuntime().lcdVideoSnapshot();
                    const juce::Point<float> position { 1402.5f + 66.0f, y };
                    require(panel->getComponentAt(position.roundToInt()) == panel,
                            "Panel control is intercepted by a child component");
                    const auto time = juce::Time::getCurrentTime();
                    const auto source = juce::Desktop::getInstance().getMainMouseSource();
                    const juce::MouseEvent down {
                        source, position, juce::ModifierKeys { juce::ModifierKeys::leftButtonModifier },
                        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, panel, panel,
                        time, position, time, 1, false
                    };
                    panel->mouseDown(down);
                    panel->mouseUp(down);
                    for (int block = 0; block < 1000; ++block)
                    {
                        audio.clear();
                        processor->processBlock(audio, midi);
                    }
                    require(processor->getMasterFirmwareRuntime().lcdVideoSnapshot() != before,
                            "Public firmware mode button did not change the LCD");
                }
                std::cout << "Public firmware mode buttons passed" << std::endl;
                std::cout << "Performance count before SET import: "
                          << processor->getNumPrograms() << std::endl;
                if (const auto* setup = std::getenv("WAVE_FACTORY_SET"))
                {
                    const auto* imagePath = std::getenv("WAVE_TEST_DISK_IMAGE");
                    require(imagePath != nullptr, "SET test needs a local disk image destination");
                    require(processor->createDiskImageFromWaveSetup(
                                juce::File(setup), juce::File(imagePath)).wasOk(),
                            "Could not create/mount the local SET test disk");
                    const auto render = [&](int count) {
                        for (int block = 0; block < count; ++block)
                        {
                            audio.clear();
                            processor->processBlock(audio, midi);
                        }
                    };
                    render(64);
                    const auto ok = wave::panel::matrixIndexForDiagnosticCode(70);
                    processor->setPanelButton(ok, true);
                    render(8);
                    processor->setPanelButton(ok, false);
                    render(1000);
                    require(processor->getNumPrograms() == 256
                                && processor->getPanelSelectedMode() == 39,
                            "SET import did not enable the Performance bank");
                    processor->setCurrentProgram(0);
                    render(1000);
                    for (const auto code : { 72, 69 })
                    {
                        const auto matrix = wave::panel::matrixIndexForDiagnosticCode(code);
                        processor->setPanelButton(matrix, true);
                        render(8);
                        processor->setPanelButton(matrix, false);
                        render(1000);
                        const auto expected = code == 72 ? 1 : 0;
                        require(processor->getCurrentProgram() == expected
                                    && processor->getMasterFirmwareRuntime().currentPerformanceId()
                                           == expected,
                                "Performance +/- did not select the adjacent firmware program");
                    }
                    std::cout << "Imported SET: Performance +/- selected A002 then A001" << std::endl;
                }
                const auto& runtime = processor->getMasterFirmwareRuntime();
                const auto video = runtime.lcdVideoSnapshot();
                wave::ui::LcdFramebuffer framebuffer;
                framebuffer.loadHardwareVideoRam(video.data(), video.size(), runtime.lcdDisplayPage());
                int firmwarePixels = 0;
                for (int y = 0; y < wave::ui::LcdFramebuffer::height; ++y)
                    for (int x = 0; x < wave::ui::LcdFramebuffer::width; ++x)
                        firmwarePixels += framebuffer.pixel(x, y) ? 1 : 0;
                std::cout << "Firmware LCD: " << firmwarePixels << " pixels, "
                          << runtime.lcdVideoWriteCount() << " writes" << std::endl;
                require(firmwarePixels > 0, "Firmware LCD framebuffer is blank");
                wave::ui::WaveLcdComponent lcd(*processor);
                lcd.setPanelEmbedded(true);
                lcd.setSize(480, 64);
                for (const auto software : { false, true })
                {
                    const auto image = software
                        ? lcd.createComponentSnapshot(lcd.getLocalBounds(), true, 1.0f,
                                                      juce::SoftwareImageType{})
                        : lcd.createComponentSnapshot(lcd.getLocalBounds());
                    int paintedPixels = 0;
                    for (int y = 0; y < image.getHeight(); ++y)
                        for (int x = 0; x < image.getWidth(); ++x)
                        {
                            const auto pixel = image.getPixelAt(x, y);
                            paintedPixels += pixel.getRed() < 40 && pixel.getGreen() < 100 ? 1 : 0;
                        }
                    std::cout << (software ? "Software" : "Native") << " LCD image: "
                              << paintedPixels << " pixels" << std::endl;
                    if (const auto* output = std::getenv("WAVE_TEST_ARTIFACT_DIR"))
                    {
                        auto stream = juce::File(output).getChildFile(
                            software ? "lcd-software.png" : "lcd-native.png").createOutputStream();
                        require(stream != nullptr && juce::PNGImageFormat{}.writeImageToStream(image, *stream),
                                "Cannot save LCD test image");
                    }
                    require(paintedPixels == firmwarePixels, "LCD renderer lost firmware pixels");
                }
            }
        };
        const auto liveFirmwareLoad = std::getenv("WAVE_TEST_LIVE_FIRMWARE_LOAD") != nullptr;
        if (!liveFirmwareLoad)
            loadAndCheckFirmware();
        if (std::getenv("WAVE_TEST_NATIVE_WINDOW") != nullptr)
        {
            std::cout << "Creating native window" << std::endl;
            juce::DocumentWindow window("Wave Windows display test",
                                        juce::Colours::darkgrey,
                                        juce::DocumentWindow::allButtons);
            std::cout << "Setting native title bar" << std::endl;
            window.setUsingNativeTitleBar(true);
            std::cout << "Attaching editor" << std::endl;
            window.setContentNonOwned(editor.get(), true);
            std::cout << "Positioning window" << std::endl;
            window.setTopLeftPosition(40, 40);
            std::cout << "Showing window" << std::endl;
            window.setVisible(true);
            require(window.getPeer() != nullptr && window.isShowing(),
                    "Native editor window is not showing");
            std::cout << "Detected " << juce::Desktop::getInstance().getDisplays().displays.size()
                      << " display(s); native window bounds "
                      << window.getScreenBounds().toString() << std::endl;
            RepaintProbe repaintProbe;
            repaintProbe.setBounds(5, 40, 8, 8);
            editor->addAndMakeVisible(repaintProbe);
            int paintsBeforeRequest = -1;
            juce::Timer::callAfterDelay(4000, [&] {
                paintsBeforeRequest = repaintProbe.paintCount;
                repaintProbe.repaint();
            });
            juce::String firmwareLoadError;
            if (liveFirmwareLoad)
                juce::Timer::callAfterDelay(1000, [&] {
                    try { loadAndCheckFirmware(); }
                    catch (const std::exception& error)
                    {
                        firmwareLoadError = error.what();
                        juce::MessageManager::getInstance()->stopDispatchLoop();
                    }
                });
            juce::Timer::callAfterDelay(10000, [] {
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
            juce::MessageManager::getInstance()->runDispatchLoop();
            require(firmwareLoadError.isEmpty(), firmwareLoadError.toRawUTF8());
            std::cout << "Native paints before/after repaint request: "
                      << paintsBeforeRequest << "/" << repaintProbe.paintCount << std::endl;
            require(paintsBeforeRequest >= 0 && repaintProbe.paintCount > paintsBeforeRequest,
                    "Native window stopped repainting after its initial frame");
            if (std::getenv("WAVE_FIRMWARE_DIR") != nullptr)
                for (int index = 0; index < editor->getNumChildComponents(); ++index)
                    if (auto* lcd = dynamic_cast<wave::ui::WaveLcdComponent*>(editor->getChildComponent(index)))
                    {
                        const auto image = lcd->createComponentSnapshot(lcd->getLocalBounds(), true, 1.0f,
                                                                        juce::SoftwareImageType{});
                        int darkPixels = 0;
                        for (int y = 0; y < image.getHeight(); ++y)
                            for (int x = 0; x < image.getWidth(); ++x)
                            {
                                const auto pixel = image.getPixelAt(x, y);
                                darkPixels += pixel.getRed() < 40 && pixel.getGreen() < 100 ? 1 : 0;
                            }
                        std::cout << "Live editor LCD: " << darkPixels << " pixels" << std::endl;
                        if (const auto* output = std::getenv("WAVE_TEST_ARTIFACT_DIR"))
                        {
                            auto stream = juce::File(output).getChildFile("lcd-live-editor.png").createOutputStream();
                            require(stream != nullptr && juce::PNGImageFormat{}.writeImageToStream(image, *stream),
                                    "Cannot save live LCD test image");
                        }
                        require(darkPixels > 0, "Live editor LCD is blank after firmware loading");
                    }
            window.clearContentComponent();
        }
        processor->releaseResources();
        std::cout << "Public editor rendering, shortcuts and reopening passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
