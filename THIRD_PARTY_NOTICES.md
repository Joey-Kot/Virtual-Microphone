# Third-Party Notices

The release package includes a GPL-licensed RNNoise VST2 plugin built from the
vendored sources below. It also includes the complete license texts in
`THIRD_PARTY_LICENSES/`. Corresponding source is available in this repository
under `vendor/noise-suppression-for-voice/`.

## noise-suppression-for-voice

- Version: vendored snapshot (default build version 1.99)
- Source: https://github.com/werman/noise-suppression-for-voice
- License: GPL-3.0-or-later
- Full license text: `THIRD_PARTY_LICENSES/noise-suppression-for-voice-GPL-3.0-or-later.txt`
- Distributed component: `rnnoise_mono.dll`

## RNNoise

- Version: vendored commit `372f7b4b76cde4ca1ec4605353dd17898a99de38`
- Source: https://github.com/xiph/rnnoise
- License: BSD-3-Clause
- Full license text: `THIRD_PARTY_LICENSES/RNNoise-BSD-3-Clause.txt`
- Build configuration: `vendor/noise-suppression-for-voice/external/rnnoise/CMakeLists.txt`

## FST

- Version: vendored commit `58a8e128229c242cafef8d169b11ace750f1fbc6`
- Source: https://git.iem.at/zmoelnig/FST.git
- License: GPL-3.0-or-later
- Full license text: `THIRD_PARTY_LICENSES/FST-GPL-3.0-or-later.txt`
- Usage: VST2 API headers used to build `rnnoise_mono.dll`

## JUCE

- Version: 7.0.1
- Source: https://github.com/juce-framework/JUCE
- License: GPL-3.0-or-later for this GPL distribution
- Full license text: `THIRD_PARTY_LICENSES/JUCE-GPL-3.0-or-later.txt`
- Usage: framework used to build `rnnoise_mono.dll`

## Project License

- GPL-3.0-or-later
