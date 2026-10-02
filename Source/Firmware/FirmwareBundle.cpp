#include "FirmwareBundle.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cstring>

namespace wave::firmware
{
namespace
{
constexpr size_t os1700MasterSize = 302768;
constexpr size_t voiceDriverSize = 12840;
} // namespace

Bundle::Report Bundle::load(const juce::File& directoryOrImage)
{
    clear();

    // A missing path must not fall back to its parent: the recursive image
    // search below would then walk e.g. the whole filesystem for "/nonexistent".
    const auto root = directoryOrImage.existsAsFile()
                          ? directoryOrImage.getParentDirectory()
                          : directoryOrImage;
    const auto masterFile = findImage(root, "w2sys.bin");
    const auto voiceFile = findImage(root, "wdv.sys");

    if (!masterFile.existsAsFile() || !voiceFile.existsAsFile())
    {
        report.summary = "FIRMWARE MISSING";
        report.detail = "Select a folder containing w2sys.bin and wdv.sys.";
        return report;
    }

    if (!masterFile.loadFileAsData(masterImage) || !voiceFile.loadFileAsData(voiceImage))
    {
        clear();
        report.summary = "FIRMWARE READ ERROR";
        report.detail = "Both files were found, but at least one could not be read.";
        return report;
    }

    directory = root;
    return validateLoadedImages();
}

Bundle::Report Bundle::loadImages(const juce::MemoryBlock& master,
                                  const juce::MemoryBlock& voice,
                                  const juce::File& sourceDirectory)
{
    clear();
    masterImage = master;
    voiceImage = voice;
    directory = sourceDirectory;
    return validateLoadedImages();
}

Bundle::Report Bundle::validateLoadedImages()
{
    if (masterImage.isEmpty() || voiceImage.isEmpty())
    {
        clear();
        report.summary = "FIRMWARE MISSING";
        report.detail = "Both master and voice images are required.";
        return report;
    }

    report.masterSha256 = sha256(masterImage);
    report.voiceSha256 = sha256(voiceImage);
    report.version = detectVersion(masterImage);

    const auto exact1700 = masterImage.getSize() == os1700MasterSize
                           && voiceImage.getSize() == voiceDriverSize
                           && report.masterSha256 == knownMasterHash
                           && report.voiceSha256 == knownVoiceHash;

    if (exact1700)
    {
        report.authenticity = Authenticity::verifiedOs1700;
        report.summary = "OS 1.700 VERIFIED - MODEL BRIDGE";
        report.detail = "Genuine master and voice-board images authenticated. "
                        "The master image runs at its linked $001000 address, draws through genuine LCD RAM, "
                        "executes the verified post-copy WDV handoff, and shares the decoded voice transport; "
                        "undocumented oscillator behaviour remains a measured black-box proxy.";
        return report;
    }

    const auto plausibleMaster = masterImage.getSize() >= 250000
                                 && masterImage.getSize() <= 400000
                                 && containsAscii(masterImage, "WAVE Operating System");
    const auto plausibleVoice = voiceImage.getSize() == voiceDriverSize
                                && voiceImage.getSize() > 0x424
                                && static_cast<const uint8_t*>(voiceImage.getData())[0] == 0x01
                                && static_cast<const uint8_t*>(voiceImage.getData())[1] == 0x02;

    if (plausibleMaster && plausibleVoice)
    {
        report.authenticity = Authenticity::recognisedCompatible;
        report.summary = "WAVE OS RECOGNISED - UNVERIFIED";
        report.detail = "The files have the expected structure, but their hashes are not the known 1.700 pair.";
        return report;
    }

    clear();
    report.summary = "INVALID FIRMWARE";
    report.detail = "The selected files do not have the expected Wave master/voice image structure.";
    return report;
}

void Bundle::clear()
{
    directory = juce::File();
    masterImage.reset();
    voiceImage.reset();
    report = {};
}

juce::File Bundle::findImage(const juce::File& root, const juce::String& name)
{
    const auto direct = root.getChildFile(name);
    if (direct.existsAsFile())
        return direct;

    juce::Array<juce::File> matches;
    root.findChildFiles(matches, juce::File::findFiles, true, name);
    return matches.isEmpty() ? juce::File{} : matches.getFirst();
}

bool Bundle::containsAscii(const juce::MemoryBlock& image, const char* text)
{
    const auto* begin = static_cast<const uint8_t*>(image.getData());
    const auto* end = begin + image.getSize();
    const auto textLength = std::strlen(text);
    return std::search(begin, end, reinterpret_cast<const uint8_t*>(text),
                       reinterpret_cast<const uint8_t*>(text) + textLength) != end;
}

juce::String Bundle::sha256(const juce::MemoryBlock& image)
{
    return juce::SHA256(image.getData(), image.getSize()).toHexString();
}

juce::String Bundle::detectVersion(const juce::MemoryBlock& image)
{
    for (const auto* candidate : { "1.700", "1.680", "1.671", "1.668" })
        if (containsAscii(image, candidate))
            return candidate;

    return "unknown";
}
} // namespace wave::firmware
