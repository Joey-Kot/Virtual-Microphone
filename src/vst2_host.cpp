#include "vst2_host.h"

#include "fst.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <stdexcept>

namespace {

struct HostSettings {
    float sampleRate = 48000.0F;
    int blockSize = 480;
};

HostSettings gHostSettings;

t_fstPtrInt VSTCALLBACK AudioMaster(AEffect*, int opcode, int, t_fstPtrInt, void*, float) {
    switch (opcode) {
    case audioMasterVersion:
        return kVstVersion;
    case audioMasterGetSampleRate:
        return static_cast<t_fstPtrInt>(gHostSettings.sampleRate);
    case audioMasterGetBlockSize:
        return gHostSettings.blockSize;
    case audioMasterGetCurrentProcessLevel:
        return kVstProcessLevelRealtime;
    case audioMasterCanDo:
        return 0;
    default:
        return 0;
    }
}

using VstPluginMain = AEffect* (*)(AEffectDispatcherProc);

void Dispatch(AEffect* effect, int opcode, int index = 0, t_fstPtrInt value = 0,
              void* pointer = nullptr, float option = 0.0F) {
    if (effect->dispatcher) {
        effect->dispatcher(effect, opcode, index, value, pointer, option);
    }
}

} // namespace

Vst2Plugin::Vst2Plugin(const std::wstring& path, unsigned sampleRate, unsigned blockSize,
                       float vadThreshold, unsigned vadGracePeriod,
                       unsigned retroactiveVadGracePeriod) {
    gHostSettings.sampleRate = static_cast<float>(sampleRate);
    gHostSettings.blockSize = static_cast<int>(blockSize);

    module_ = LoadLibraryW(path.c_str());
    if (!module_) {
        throw std::runtime_error("could not load VST2 plugin DLL");
    }
    auto entry = reinterpret_cast<VstPluginMain>(GetProcAddress(module_, "VSTPluginMain"));
    if (!entry) {
        FreeLibrary(module_);
        module_ = nullptr;
        throw std::runtime_error("the DLL does not export VSTPluginMain");
    }
    effect_ = entry(AudioMaster);
    const bool compatibleChannels = effect_ && effect_->numInputs == effect_->numOutputs &&
                                    (effect_->numInputs == 1 || effect_->numInputs == 2);
    if (!effect_ || effect_->magic != 0x56737450 || !effect_->processReplacing || !compatibleChannels) {
        std::ostringstream detail;
        detail << "the VST2 plugin is not a compatible mono or stereo audio effect";
        if (effect_) {
            detail << " (magic=0x" << std::hex << static_cast<std::uint32_t>(effect_->magic)
                   << std::dec << ", inputs=" << effect_->numInputs
                   << ", outputs=" << effect_->numOutputs
                   << ", processReplacing=" << (effect_->processReplacing ? "yes" : "no") << ')';
        }
        if (effect_ && effect_->dispatcher) {
            Dispatch(effect_, effClose);
        }
        FreeLibrary(module_);
        module_ = nullptr;
        effect_ = nullptr;
        throw std::runtime_error(detail.str());
    }

    channels_ = effect_->numInputs;

    Dispatch(effect_, effOpen);
    Dispatch(effect_, effSetSampleRate, 0, 0, nullptr, static_cast<float>(sampleRate));
    Dispatch(effect_, effSetBlockSize, 0, static_cast<t_fstPtrInt>(blockSize));
    effect_->setParameter(effect_, 0, std::clamp(vadThreshold, 0.0F, 1.0F));
    effect_->setParameter(effect_, 1, std::clamp(static_cast<float>(vadGracePeriod) / 500.0F, 0.0F, 1.0F));
    effect_->setParameter(effect_, 2,
                          std::clamp(static_cast<float>(retroactiveVadGracePeriod) / 10.0F, 0.0F, 1.0F));
    Dispatch(effect_, effMainsChanged, 0, 1);
    Dispatch(effect_, effStartProcess);
}

Vst2Plugin::~Vst2Plugin() {
    if (effect_) {
        Dispatch(effect_, effStopProcess);
        Dispatch(effect_, effMainsChanged, 0, 0);
        Dispatch(effect_, effClose);
    }
    if (module_) {
        FreeLibrary(module_);
    }
}

void Vst2Plugin::Process(std::vector<float>& samples) {
    if (samples.empty()) {
        return;
    }
    input_.resize(channels_);
    output_.resize(channels_);
    inputPointers_.resize(channels_);
    outputPointers_.resize(channels_);
    for (int channel = 0; channel < channels_; ++channel) {
        // A physical microphone is generally mono. Duplicate it for a stereo
        // RNNoise build so each channel gets an identical, independent state.
        input_[channel] = samples;
        output_[channel].resize(samples.size());
        inputPointers_[channel] = input_[channel].data();
        outputPointers_[channel] = output_[channel].data();
    }
    effect_->processReplacing(effect_, inputPointers_.data(), outputPointers_.data(),
                              static_cast<int>(samples.size()));
    samples.swap(output_[0]);
}
