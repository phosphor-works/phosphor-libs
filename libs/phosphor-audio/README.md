<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-audio

phosphor-audio supplies an audio spectrum to audio-reactive shaders and overlays. Its bundled provider runs the
`cava` program of the user and emits the spectrum as a vector of bars.

## Responsibility

The library is a small audio-spectrum feed for shader effects and QML overlays. It has one contract
(`IAudioSpectrumProvider`) and one bundled implementation, which starts the `cava` program that the user installed.
Consumers send the emitted bar vector into a `ShaderEffect` UBO.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorAudio::IAudioSpectrumProvider` | Provider contract: `start`, `stop`, `options()`/`setOptions()` (the full `SpectrumOptions` parameter set), `spectrum()` snapshot. |
| `PhosphorAudio::CavaSpectrumProvider`   | Provider that uses `cava`. It detects the install and selects PipeWire or PulseAudio automatically. Other backends, such as ALSA, use the `inputMethod` override. It generates a config, gives it to `cava` on standard input (`-p /dev/stdin`) and emits normalized FFT bars. |
| `PhosphorAudio::SpectrumOptions`        | The options that a provider runs with. `AudioDefaults.h` gives the defaults and limits. |

## Use

```cpp
auto* provider = new PhosphorAudio::CavaSpectrumProvider(parent);
PhosphorAudio::SpectrumOptions options;
options.barCount = 64;
options.framerate = 60;
provider->setOptions(options);
QObject::connect(provider, &PhosphorAudio::IAudioSpectrumProvider::spectrumUpdated,
                 shaderEffect, &MyShader::setBars);
provider->start();
```

## Option limits

`CavaSpectrumProvider::normalizedOptions()` clamps each option to these limits from `AudioDefaults.h`. It also rounds
an odd bar count up to the next even number. Call it to see the values that the provider will use.

| Option | Default | Minimum | Maximum |
|--------|---------|---------|---------|
| `barCount` | 64 | 16 | 256 |
| `framerate` | 60 | 30 | 144 |
| `sensitivity` (percent) | 100 | 10 | 500 |
| `noiseReduction` | 77 | 0 | 100 |
| `lowerCutoffHz` (Hz) | 50 | 20 | 500 |
| `higherCutoffHz` (Hz) | 10000 | 1000 | 20000 |
| `extraSmoothing` | 0.5 | 0.0 | 0.95 |

## Design notes

- **No direct audio backend.** `CavaSpectrumProvider` starts `cava`, which owns the audio capture and the FFT. The
  library selects PipeWire or PulseAudio (or uses an explicit `inputMethod` override) for the generated config, and
  parses the framed byte output of cava.
- **Graceful degradation.** `isAvailable()` returns false when `cava` is not installed. In that case, consumers
  must hide or disable audio-reactive overlays and not fail.

## Dependencies

- `QtCore`

## See also

- [`phosphor-shaders`](../phosphor-shaders/README.md): the spectrum goes into shader UBOs through
  `IUniformExtension`.
- [`phosphor-rendering`](../phosphor-rendering/README.md): the host item that consumes the spectrum.
