// WASAPI device enumeration and shared-mode capture / render helpers.
#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace babelvoice {

using Microsoft::WRL::ComPtr;

struct Endpoint {
  std::wstring id;
  std::string uid;
  std::string name;
  bool input = false;
  bool isDefault = false;
  bool alive = false;
  double sampleRate = 0;
  int channels = 0;
  bool isVirtual = false;
};

struct ComApartment {
  ComApartment();
  ~ComApartment();
  bool ok = false;
  bool shouldUninitialize = false;
};

struct WasapiClient {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  ComPtr<IAudioRenderClient> render;
  WAVEFORMATEX *mix = nullptr;
  // The client format is 16-bit mono at `sampleRate` (WASAPI converts).
  bool pcm16Mono = false;
  int sampleRate = 0;
  int channels = 1;
  HANDLE event = nullptr;
  WasapiClient() = default;
  ~WasapiClient();
  void Close();
  WasapiClient(const WasapiClient &) = delete;
  WasapiClient &operator=(const WasapiClient &) = delete;
};

std::string WideToUtf8(const wchar_t *wide);
std::wstring Utf8ToWide(const std::string &utf8);
std::string HresultText(const char *what, HRESULT hr);
bool NameMatches(const std::string &name, const std::string &pattern);
bool IsVirtualDeviceName(const std::string &name);

// Playback side of VB-Cable ("CABLE Input") or Voicemeeter Input.
constexpr const char *kVirtualOutputPattern = "cable input|voicemeeter input|vb-audio virtual cable";

std::vector<Endpoint> ListEndpoints();
bool HasVirtualOutput();
const char *MicConsent();

HRESULT DefaultDevice(EDataFlow flow, ComPtr<IMMDevice> &device);
HRESULT DeviceFromId(const std::wstring &id, ComPtr<IMMDevice> &device);
std::string DeviceFriendlyName(IMMDevice *device);

// `extraFlags` is OR'd in (LOOPBACK, EVENTCALLBACK). Asks for 16-bit mono at
// `targetRate` and falls back to the device mix format.
HRESULT OpenCapture(IMMDevice *device, DWORD extraFlags, int targetRate, WasapiClient &out);
HRESULT OpenProcessCapture(DWORD pid, int targetRate, WasapiClient &out);
HRESULT OpenRender(const std::string &pattern, int targetRate, WasapiClient &out, std::string &deviceName);

// Reads one capture packet into mono int16 at the client rate.
// S_OK and frames==0 means the queue is empty. A failed HRESULT means the
// device went away or the client stopped.
HRESULT ReadCapture(WasapiClient &client, std::vector<int16_t> &out, uint32_t &frames);
// Writes as many frames as the render buffer can take. Returns frames written.
uint32_t PushMonoS16(WasapiClient &client, const int16_t *samples, uint32_t frames);

}  // namespace babelvoice
