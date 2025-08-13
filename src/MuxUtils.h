#pragma once
#include <juce_core/juce_core.h>

bool juceMuxAudioVideo(const juce::File& audioWav, const juce::File& videoMov, const juce::File& outMov, juce::String& errorOut);
