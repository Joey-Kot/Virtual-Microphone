#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

struct _fstEffect;

class Vst2Plugin final {
public:
    Vst2Plugin(const std::wstring& path, unsigned sampleRate, unsigned blockSize,
               float vadThreshold, unsigned vadGracePeriod, unsigned retroactiveVadGracePeriod);
    ~Vst2Plugin();

    Vst2Plugin(const Vst2Plugin&) = delete;
    Vst2Plugin& operator=(const Vst2Plugin&) = delete;

    void Process(std::vector<float>& samples);

private:
    HMODULE module_ = nullptr;
    _fstEffect* effect_ = nullptr;
    int channels_ = 0;
    std::vector<std::vector<float>> input_;
    std::vector<std::vector<float>> output_;
    std::vector<float*> inputPointers_;
    std::vector<float*> outputPointers_;
};
