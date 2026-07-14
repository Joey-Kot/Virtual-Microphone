#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include "vst2_host.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kDefaultConfigName[] = L"virtual-microphone.ini";
// PKEY_Device_FriendlyName. Define it locally because some MinGW-w64 SDK
// distributions declare the key but do not provide a linkable definition.
constexpr PROPERTYKEY kDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14,
};

struct Config {
    std::wstring input;
    std::wstring output;
    std::wstring vstPlugin;
    double gainDb = 20.0;
    float vadThreshold = 0.6F;
    unsigned vadGracePeriod = 20;
    unsigned retroactiveVadGracePeriod = 0;
};

// std::wcout is converted through the active C locale, which makes Chinese
// endpoint names disappear in some PowerShell console configurations. Write
// UTF-16 directly to a Windows console; write UTF-8 when stdout/stderr was
// redirected to a file or another process.
void Print(std::wstring_view text, bool error = false) {
    const HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD consoleMode = 0;
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr && GetConsoleMode(handle, &consoleMode)) {
        DWORD written = 0;
        WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }

    const int byteCount = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                              nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0) {
        return;
    }
    std::string utf8(static_cast<std::size_t>(byteCount), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(),
                        byteCount, nullptr, nullptr);
    std::fwrite(utf8.data(), 1, utf8.size(), error ? stderr : stdout);
}

[[noreturn]] void Fail(std::wstring_view message, HRESULT hr = S_OK) {
    std::wostringstream output;
    output << L"Error: " << message;
    if (FAILED(hr)) {
        output << L" (0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << L")";
    }
    output << L'\n';
    Print(output.str(), true);
    throw std::runtime_error("virtual-microphone failed");
}

void Check(HRESULT hr, std::wstring_view action) {
    if (FAILED(hr)) {
        Fail(action, hr);
    }
}

std::wstring Trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

std::wstring ReadFriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    Check(device->OpenPropertyStore(STGM_READ, &properties), L"opening endpoint properties");

    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT hr = properties->GetValue(kDeviceFriendlyName, &value);
    if (FAILED(hr)) {
        PropVariantClear(&value);
        Check(hr, L"reading endpoint friendly name");
    }

    std::wstring result = value.vt == VT_LPWSTR && value.pwszVal ? value.pwszVal : L"(unnamed endpoint)";
    PropVariantClear(&value);
    return result;
}

std::wstring ReadId(IMMDevice* device) {
    LPWSTR rawId = nullptr;
    Check(device->GetId(&rawId), L"reading endpoint ID");
    std::wstring id(rawId);
    CoTaskMemFree(rawId);
    return id;
}

void ListDevices(IMMDeviceEnumerator* enumerator, EDataFlow flow) {
    ComPtr<IMMDeviceCollection> devices;
    Check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices), L"enumerating endpoints");

    UINT count = 0;
    Check(devices->GetCount(&count), L"counting endpoints");
    Print(flow == eCapture ? L"Capture devices:\n" : L"Render devices:\n");
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        Check(devices->Item(index, &device), L"opening endpoint");
        Print(L"  Name: " + ReadFriendlyName(device.Get()) + L"\n" +
              L"  ID:   " + ReadId(device.Get()) + L"\n\n");
    }
}

ComPtr<IMMDevice> SelectDevice(IMMDeviceEnumerator* enumerator, EDataFlow flow,
                               const std::wstring& selector) {
    if (selector.empty()) {
        Fail(flow == eCapture ? L"input is missing from the configuration" : L"output is missing from the configuration");
    }

    // Endpoint IDs are unambiguous and should be preferred. A friendly-name substring
    // is accepted for convenience, but it must match exactly one active endpoint.
    ComPtr<IMMDevice> exact;
    if (SUCCEEDED(enumerator->GetDevice(selector.c_str(), &exact))) {
        return exact;
    }

    ComPtr<IMMDeviceCollection> devices;
    Check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices), L"enumerating endpoints");
    UINT count = 0;
    Check(devices->GetCount(&count), L"counting endpoints");

    const auto needle = Lower(selector);
    ComPtr<IMMDevice> match;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> candidate;
        Check(devices->Item(index, &candidate), L"opening endpoint");
        if (Lower(ReadFriendlyName(candidate.Get())).find(needle) == std::wstring::npos) {
            continue;
        }
        if (match) {
            Fail(L"the endpoint selector matches more than one device; use the exact endpoint ID");
        }
        match = candidate;
    }

    if (!match) {
        Fail(L"no active endpoint matches the configured selector");
    }
    return match;
}

Config LoadConfig(const std::wstring& path) {
    std::wifstream file(path.c_str());
    if (!file) {
        Fail(L"cannot open configuration file: " + path);
    }

    Config config;
    std::wstring line;
    unsigned lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        line = Trim(line);
        if (line.empty() || line.starts_with(L"#") || line.starts_with(L";")) {
            continue;
        }
        const auto separator = line.find(L'=');
        if (separator == std::wstring::npos) {
            Fail(L"invalid configuration line " + std::to_wstring(lineNumber));
        }
        const auto key = Lower(Trim(line.substr(0, separator)));
        const auto value = Trim(line.substr(separator + 1));
        if (key == L"input") {
            config.input = value;
        } else if (key == L"output") {
            config.output = value;
        } else if (key == L"gain_db") {
            try {
                config.gainDb = std::stod(value);
            } catch (const std::exception&) {
                Fail(L"gain_db must be a number");
            }
        } else if (key == L"vst_plugin") {
            config.vstPlugin = value;
        } else if (key == L"vad_threshold") {
            try {
                config.vadThreshold = std::stof(value);
            } catch (const std::exception&) {
                Fail(L"vad_threshold must be a number");
            }
        } else if (key == L"vad_grace_period") {
            try {
                config.vadGracePeriod = static_cast<unsigned>(std::stoul(value));
            } catch (const std::exception&) {
                Fail(L"vad_grace_period must be a whole number");
            }
        } else if (key == L"vad_retroactive_grace_period") {
            try {
                config.retroactiveVadGracePeriod = static_cast<unsigned>(std::stoul(value));
            } catch (const std::exception&) {
                Fail(L"vad_retroactive_grace_period must be a whole number");
            }
        } else {
            Fail(L"unknown configuration key: " + key);
        }
    }
    if (!std::isfinite(config.gainDb) || config.gainDb < -96.0 || config.gainDb > 60.0) {
        Fail(L"gain_db must be between -96 and 60");
    }
    if (!std::isfinite(config.vadThreshold) || config.vadThreshold < 0.0F || config.vadThreshold > 1.0F ||
        config.vadGracePeriod > 500 || config.retroactiveVadGracePeriod > 10) {
        Fail(L"VST VAD parameters are outside the plugin's supported range");
    }
    return config;
}

bool IsFloatFormat(const WAVEFORMATEX& format) {
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        return extensible.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

bool IsPcmFormat(const WAVEFORMATEX& format) {
    if (format.wFormatTag == WAVE_FORMAT_PCM) {
        return true;
    }
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        return extensible.SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
    }
    return false;
}

int ValidBits(const WAVEFORMATEX& format) {
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).Samples.wValidBitsPerSample;
    }
    return format.wBitsPerSample;
}

std::int32_t ReadPcm24(const std::byte* p) {
    std::int32_t sample = static_cast<std::uint8_t>(p[0]) |
                          (static_cast<std::uint8_t>(p[1]) << 8) |
                          (static_cast<std::uint8_t>(p[2]) << 16);
    if (sample & 0x00800000) {
        sample |= ~0x00ffffff;
    }
    return sample;
}

void WritePcm24(std::byte* p, std::int32_t sample) {
    p[0] = static_cast<std::byte>(sample & 0xff);
    p[1] = static_cast<std::byte>((sample >> 8) & 0xff);
    p[2] = static_cast<std::byte>((sample >> 16) & 0xff);
}

void ValidateFormat(const WAVEFORMATEX& format) {
    if (format.nChannels == 0 || format.nSamplesPerSec == 0 ||
        (!IsFloatFormat(format) && !IsPcmFormat(format))) {
        Fail(L"the endpoint uses an unsupported audio format");
    }
    if (IsFloatFormat(format) && format.wBitsPerSample == 32) {
        return;
    }
    if (IsPcmFormat(format) && (format.wBitsPerSample == 16 || format.wBitsPerSample == 24 ||
                                format.wBitsPerSample == 32)) {
        return;
    }
    Fail(L"only 16/24/32-bit PCM and 32-bit float endpoint formats are supported");
}

float ReadSample(const std::byte* data, UINT32 frame, WORD channel, const WAVEFORMATEX& format) {
    const auto bytesPerSample = static_cast<std::size_t>(format.wBitsPerSample / 8);
    const auto* sample = data + static_cast<std::size_t>(frame) * format.nBlockAlign +
                         static_cast<std::size_t>(channel) * bytesPerSample;
    if (IsFloatFormat(format)) {
        float value = 0.0F;
        std::memcpy(&value, sample, sizeof(value));
        return std::clamp(value, -1.0F, 1.0F);
    }

    const int validBits = ValidBits(format);
    std::int64_t value = 0;
    if (format.wBitsPerSample == 16) {
        std::int16_t raw = 0;
        std::memcpy(&raw, sample, sizeof(raw));
        value = raw;
    } else if (format.wBitsPerSample == 24) {
        value = ReadPcm24(sample);
    } else {
        std::int32_t raw = 0;
        std::memcpy(&raw, sample, sizeof(raw));
        value = raw;
    }
    if (format.wBitsPerSample > validBits) {
        value >>= format.wBitsPerSample - validBits;
    }
    return static_cast<float>(value / static_cast<double>(std::int64_t{1} << (validBits - 1)));
}

void WriteSample(std::byte* data, UINT32 frame, WORD channel, const WAVEFORMATEX& format, float value) {
    const auto bytesPerSample = static_cast<std::size_t>(format.wBitsPerSample / 8);
    auto* sample = data + static_cast<std::size_t>(frame) * format.nBlockAlign +
                   static_cast<std::size_t>(channel) * bytesPerSample;
    value = std::clamp(value, -1.0F, 1.0F);
    if (IsFloatFormat(format)) {
        std::memcpy(sample, &value, sizeof(value));
        return;
    }

    const int validBits = ValidBits(format);
    auto integer = static_cast<std::int64_t>(std::llround(value * ((std::int64_t{1} << (validBits - 1)) - 1)));
    integer <<= format.wBitsPerSample - validBits;
    if (format.wBitsPerSample == 16) {
        const auto raw = static_cast<std::int16_t>(integer);
        std::memcpy(sample, &raw, sizeof(raw));
    } else if (format.wBitsPerSample == 24) {
        WritePcm24(sample, static_cast<std::int32_t>(integer));
    } else {
        const auto raw = static_cast<std::int32_t>(integer);
        std::memcpy(sample, &raw, sizeof(raw));
    }
}

struct ResampleState {
    double sourceFrame = 0.0;
};

UINT32 ConvertAudio(const std::byte* input, UINT32 inputFrames, const WAVEFORMATEX& inputFormat,
                    std::byte* output, const WAVEFORMATEX& outputFormat, bool silent,
                    ResampleState& state) {
    const double sourceFramesPerOutputFrame = static_cast<double>(inputFormat.nSamplesPerSec) /
                                               outputFormat.nSamplesPerSec;
    UINT32 outputFrames = 0;
    while (state.sourceFrame < inputFrames) {
        const auto sourceFrame = static_cast<UINT32>(state.sourceFrame);
        for (WORD outputChannel = 0; outputChannel < outputFormat.nChannels; ++outputChannel) {
            // Mono is duplicated to all output channels. When reducing channels,
            // retain the corresponding leading source channels rather than silently
            // discarding a mono microphone because the virtual cable is stereo.
            const WORD inputChannel = std::min<WORD>(outputChannel, inputFormat.nChannels - 1);
            const float source = silent ? 0.0F : ReadSample(input, sourceFrame, inputChannel, inputFormat);
            WriteSample(output, outputFrames, outputChannel, outputFormat, source);
        }
        ++outputFrames;
        state.sourceFrame += sourceFramesPerOutputFrame;
    }
    state.sourceFrame -= inputFrames;
    return outputFrames;
}

void ApplyVstAndGain(std::byte* data, UINT32 frames, const WAVEFORMATEX& format, double gain,
                     Vst2Plugin* vstPlugin) {
    if (!vstPlugin) {
        for (UINT32 frame = 0; frame < frames; ++frame) {
            for (WORD channel = 0; channel < format.nChannels; ++channel) {
                WriteSample(data, frame, channel, format,
                            static_cast<float>(ReadSample(data, frame, channel, format) * gain));
            }
        }
        return;
    }

    std::vector<float> mono(frames);
    for (UINT32 frame = 0; frame < frames; ++frame) {
        mono[frame] = ReadSample(data, frame, 0, format);
    }
    vstPlugin->Process(mono);
    for (UINT32 frame = 0; frame < frames; ++frame) {
        const float sample = static_cast<float>(mono[frame] * gain);
        for (WORD channel = 0; channel < format.nChannels; ++channel) {
            WriteSample(data, frame, channel, format, sample);
        }
    }
}

void Run(const Config& config) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(&enumerator)),
          L"creating device enumerator");

    auto input = SelectDevice(enumerator.Get(), eCapture, config.input);
    auto output = SelectDevice(enumerator.Get(), eRender, config.output);
    std::wostringstream startup;
    startup << L"Input:  " << ReadFriendlyName(input.Get()) << L"\n"
            << L"Output: " << ReadFriendlyName(output.Get()) << L"\n"
            << L"Gain:   " << config.gainDb << L" dB\n";
    if (!config.vstPlugin.empty()) {
        startup << L"VST2:   " << config.vstPlugin << L"\n";
    }
    Print(startup.str());

    ComPtr<IAudioClient> renderClient;
    Check(output->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(renderClient.GetAddressOf())),
          L"activating render endpoint");

    WAVEFORMATEX* sharedFormatRaw = nullptr;
    Check(renderClient->GetMixFormat(&sharedFormatRaw), L"getting render mix format");
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> sharedFormat(sharedFormatRaw, CoTaskMemFree);
    ValidateFormat(*sharedFormat);
    if (!config.vstPlugin.empty() && sharedFormat->nSamplesPerSec != 48000) {
        Fail(L"RNNoise VST2 requires the virtual cable output format to be 48 kHz");
    }

    const REFERENCE_TIME bufferDuration = 100000; // 10 ms
    Check(renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0,
                                  sharedFormat.get(), nullptr),
          L"initializing render stream");

    ComPtr<IAudioClient> captureClient;
    Check(input->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(captureClient.GetAddressOf())),
          L"activating capture endpoint");

    WAVEFORMATEX* captureFormatRaw = nullptr;
    Check(captureClient->GetMixFormat(&captureFormatRaw), L"getting capture mix format");
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> captureFormat(captureFormatRaw, CoTaskMemFree);
    ValidateFormat(*captureFormat);
    Check(captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0,
                                   captureFormat.get(), nullptr),
          L"initializing capture stream");

    UINT32 renderBufferFrames = 0;
    Check(renderClient->GetBufferSize(&renderBufferFrames), L"getting render buffer size");

    ComPtr<IAudioCaptureClient> capture;
    Check(captureClient->GetService(IID_PPV_ARGS(&capture)), L"getting capture client");
    ComPtr<IAudioRenderClient> render;
    Check(renderClient->GetService(IID_PPV_ARGS(&render)), L"getting render client");

    DWORD taskIndex = 0;
    HANDLE mmcssTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    const auto gain = std::pow(10.0, config.gainDb / 20.0);
    ResampleState resampleState;
    std::unique_ptr<Vst2Plugin> vstPlugin;
    if (!config.vstPlugin.empty()) {
        try {
            vstPlugin = std::make_unique<Vst2Plugin>(config.vstPlugin, sharedFormat->nSamplesPerSec, 480,
                                                     config.vadThreshold, config.vadGracePeriod,
                                                     config.retroactiveVadGracePeriod);
        } catch (const std::exception& error) {
            Fail(L"loading VST2 plugin failed: " + std::wstring(error.what(), error.what() + std::strlen(error.what())));
        }
    }
    const auto maxOutputFrames = static_cast<UINT32>(
        std::ceil(static_cast<double>(captureFormat->nSamplesPerSec) / sharedFormat->nSamplesPerSec * 4096.0)) + 2;
    std::vector<std::byte> converted(maxOutputFrames * sharedFormat->nBlockAlign);

    Check(renderClient->Start(), L"starting render stream");
    Check(captureClient->Start(), L"starting capture stream");
    Print(L"Running. Press Ctrl+C to stop.\n");

    for (;;) {
        UINT32 packetFrames = 0;
        Check(capture->GetNextPacketSize(&packetFrames), L"reading capture packet size");
        if (packetFrames == 0) {
            Sleep(2);
            continue;
        }

        BYTE* capturedData = nullptr;
        DWORD flags = 0;
        Check(capture->GetBuffer(&capturedData, &packetFrames, &flags, nullptr, nullptr),
              L"reading capture data");

        const auto requiredBytes = static_cast<std::size_t>(
            std::ceil(static_cast<double>(packetFrames) * sharedFormat->nSamplesPerSec /
                      captureFormat->nSamplesPerSec) + 2) * sharedFormat->nBlockAlign;
        if (converted.size() < requiredBytes) {
            converted.resize(requiredBytes);
        }
        const UINT32 outputFrames = ConvertAudio(
            reinterpret_cast<const std::byte*>(capturedData), packetFrames, *captureFormat,
            converted.data(), *sharedFormat, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0,
            resampleState);
        ApplyVstAndGain(converted.data(), outputFrames, *sharedFormat, gain, vstPlugin.get());

        UINT32 padding = 0;
        Check(renderClient->GetCurrentPadding(&padding), L"reading render padding");
        const UINT32 writableFrames = renderBufferFrames - padding;
        if (outputFrames > writableFrames) {
            // This should only occur after a debugger stop or a long scheduling pause.
            // Drop the old packet instead of blocking the real-time capture stream.
            Check(capture->ReleaseBuffer(packetFrames), L"releasing dropped capture data");
            continue;
        }

        BYTE* renderData = nullptr;
        Check(render->GetBuffer(outputFrames, &renderData), L"getting render buffer");
        std::memcpy(renderData, converted.data(),
                    static_cast<std::size_t>(outputFrames) * sharedFormat->nBlockAlign);
        Check(render->ReleaseBuffer(outputFrames, 0), L"submitting render data");
        Check(capture->ReleaseBuffer(packetFrames), L"releasing capture data");
    }

    // Unreachable in normal operation; retained for completeness if the loop is later
    // changed to support a graceful stop signal.
    captureClient->Stop();
    renderClient->Stop();
    if (mmcssTask) {
        AvRevertMmThreadCharacteristics(mmcssTask);
    }
}

void PrintUsage() {
    Print(L"virtual-microphone --list-devices\n"
          L"virtual-microphone [--config PATH]\n\n"
          L"The configuration file contains input, output, and gain_db.\n");
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), L"initializing COM");
        struct ComUninitializer {
            ~ComUninitializer() { CoUninitialize(); }
        } comUninitializer;

        ComPtr<IMMDeviceEnumerator> enumerator;
        Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               IID_PPV_ARGS(&enumerator)),
              L"creating device enumerator");

        if (argc == 2 && std::wstring_view(argv[1]) == L"--list-devices") {
            ListDevices(enumerator.Get(), eCapture);
            ListDevices(enumerator.Get(), eRender);
            return 0;
        }

        std::wstring configPath = kDefaultConfigName;
        if (argc == 3 && std::wstring_view(argv[1]) == L"--config") {
            configPath = argv[2];
        } else if (argc != 1) {
            PrintUsage();
            return 2;
        }

        Run(LoadConfig(configPath));
        return 0;
    } catch (const std::exception&) {
        return 1;
    }
}
