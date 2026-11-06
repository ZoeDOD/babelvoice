#include "wasapi.h"

#include <audioclientactivationparams.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace babelvoice {
namespace {

std::string Lower(std::string value) {
  for (char &ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}

HRESULT InitClientFormat(IAudioClient *client, DWORD flags, int targetRate, WasapiClient &out) {
  WAVEFORMATEX wanted{};
  wanted.wFormatTag = WAVE_FORMAT_PCM;
  wanted.nChannels = 1;
  wanted.nSamplesPerSec = static_cast<DWORD>(targetRate);
  wanted.wBitsPerSample = 16;
  wanted.nBlockAlign = 2;
  wanted.nAvgBytesPerSec = static_cast<DWORD>(targetRate) * 2;

  const DWORD withConvert = flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  // 20 ms. Shared mode ignores alignment; 0 duration lets the engine pick.
  HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, withConvert, 200000, 0, &wanted, nullptr);
  if (SUCCEEDED(hr)) {
    out.pcm16Mono = true;
    out.sampleRate = targetRate;
    out.channels = 1;
    return S_OK;
  }

  hr = client->GetMixFormat(&out.mix);
  if (FAILED(hr)) return hr;
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, out.mix, nullptr);
  if (FAILED(hr)) return hr;
  out.pcm16Mono = false;
  out.sampleRate = static_cast<int>(out.mix->nSamplesPerSec);
  out.channels = out.mix->nChannels;
  return S_OK;
}

const WAVEFORMATEXTENSIBLE *AsExtensible(const WAVEFORMATEX *fmt) {
  if (!fmt || fmt->wFormatTag != WAVE_FORMAT_EXTENSIBLE || fmt->cbSize < 22) return nullptr;
  return reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
}

bool IsFloatFormat(const WAVEFORMATEX *fmt) {
  const auto *ext = AsExtensible(fmt);
  if (ext) return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  return fmt && fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
}

bool IsPcm16Format(const WAVEFORMATEX *fmt) {
  const auto *ext = AsExtensible(fmt);
  if (ext) return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && fmt->wBitsPerSample == 16;
  return fmt && fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->wBitsPerSample == 16;
}

void DownmixToMono(const int16_t *interleaved, int channels, uint32_t frames, std::vector<int16_t> &out) {
  out.resize(frames);
  if (channels <= 1) {
    std::memcpy(out.data(), interleaved, frames * sizeof(int16_t));
    return;
  }
  for (uint32_t i = 0; i < frames; i++) {
    int sum = 0;
    for (int c = 0; c < channels; c++) sum += interleaved[i * channels + c];
    out[i] = static_cast<int16_t>(sum / channels);
  }
}

void DownmixFloatToMono(const float *interleaved, int channels, uint32_t frames, std::vector<int16_t> &out) {
  out.resize(frames);
  for (uint32_t i = 0; i < frames; i++) {
    double sum = 0;
    const int ch = std::max(channels, 1);
    for (int c = 0; c < ch; c++) sum += interleaved[i * ch + c];
    const double sample = std::clamp(sum / ch, -1.0, 1.0);
    out[i] = static_cast<int16_t>(std::lround(sample * 32767.0));
  }
}

// Marker so ActivateAudioInterfaceAsync may call us from the thread pool
// instead of marshaling back onto a blocked STA thread.
static const GUID kAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

class ActivateHandler : public IActivateAudioInterfaceCompletionHandler {
 public:
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HRESULT hr = E_FAIL;
  ComPtr<IAudioClient> client;

  ~ActivateHandler() {
    if (done) CloseHandle(done);
  }

  STDMETHODIMP QueryInterface(REFIID riid, void **pp) override {
    if (!pp) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
        riid == kAgileObject) {
      *pp = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
      AddRef();
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG left = InterlockedDecrement(&ref_);
    if (left == 0) delete this;
    return left;
  }
  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation) override {
    IUnknown *unknown = nullptr;
    HRESULT activateHr = E_FAIL;
    hr = operation->GetActivateResult(&activateHr, &unknown);
    if (SUCCEEDED(hr)) hr = activateHr;
    if (SUCCEEDED(hr) && unknown) {
      hr = unknown->QueryInterface(IID_PPV_ARGS(&client));
      unknown->Release();
    }
    SetEvent(done);
    return S_OK;
  }

 private:
  LONG ref_ = 1;
};

std::string ReadConsent(const wchar_t *subkey) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return "";
  wchar_t buf[32] = {};
  DWORD size = sizeof(buf);
  DWORD type = 0;
  const LSTATUS status = RegQueryValueExW(key, L"Value", nullptr, &type, reinterpret_cast<BYTE *>(buf), &size);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_SZ) return "";
  return WideToUtf8(buf);
}

}  // namespace

ComApartment::ComApartment() {
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
  shouldUninitialize = hr == S_OK || hr == S_FALSE;
}

ComApartment::~ComApartment() {
  if (shouldUninitialize) CoUninitialize();
}

void WasapiClient::Close() {
  if (client) client->Stop();
  client.Reset();
  capture.Reset();
  render.Reset();
  if (mix) {
    CoTaskMemFree(mix);
    mix = nullptr;
  }
  if (event) {
    CloseHandle(event);
    event = nullptr;
  }
  pcm16Mono = false;
  sampleRate = 0;
  channels = 1;
}

WasapiClient::~WasapiClient() { Close(); }

std::string WideToUtf8(const wchar_t *wide) {
  if (!wide || !*wide) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) return {};
  std::string out(static_cast<size_t>(bytes - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(const std::string &utf8) {
  if (utf8.empty()) return {};
  const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (chars <= 0) return {};
  std::wstring out(static_cast<size_t>(chars), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), chars);
  return out;
}

std::string HresultText(const char *what, HRESULT hr) {
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
  return buf;
}

bool NameMatches(const std::string &name, const std::string &pattern) {
  const std::string haystack = Lower(name);
  size_t start = 0;
  while (start <= pattern.size()) {
    const size_t bar = pattern.find('|', start);
    const std::string part = Lower(pattern.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
    if (!part.empty() && haystack.find(part) != std::string::npos) return true;
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return false;
}

bool IsVirtualDeviceName(const std::string &name) {
  return NameMatches(name, "cable input|cable output|vb-audio|voicemeeter");
}

const char *MicConsent() {
  const wchar_t *keys[] = {
      L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone",
      L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone\\NonPackaged"};
  bool sawAllow = false;
  for (const wchar_t *key : keys) {
    const std::string value = ReadConsent(key);
    if (_stricmp(value.c_str(), "Deny") == 0) return "denied";
    if (_stricmp(value.c_str(), "Allow") == 0) sawAllow = true;
  }
  return sawAllow ? "granted" : "not-determined";
}

namespace {

HRESULT Enumerator(ComPtr<IMMDeviceEnumerator> &out) {
  return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&out));
}

void ReadMix(IMMDevice *device, double &sampleRate, int &channels) {
  ComPtr<IAudioClient> client;
  if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return;
  WAVEFORMATEX *fmt = nullptr;
  if (FAILED(client->GetMixFormat(&fmt)) || !fmt) return;
  sampleRate = fmt->nSamplesPerSec;
  channels = fmt->nChannels;
  CoTaskMemFree(fmt);
}

}  // namespace

std::string DeviceFriendlyName(IMMDevice *device) {
  if (!device) return {};
  ComPtr<IPropertyStore> store;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return {};
  PROPVARIANT value;
  PropVariantInit(&value);
  std::string name;
  if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
    name = WideToUtf8(value.pwszVal);
  }
  PropVariantClear(&value);
  return name;
}

HRESULT DefaultDevice(EDataFlow flow, ComPtr<IMMDevice> &device) {
  ComApartment apartment;
  if (!apartment.ok) return CO_E_NOTINITIALIZED;
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = Enumerator(enumerator);
  if (FAILED(hr)) return hr;
  return enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
}

HRESULT DeviceFromId(const std::wstring &id, ComPtr<IMMDevice> &device) {
  ComApartment apartment;
  if (!apartment.ok) return CO_E_NOTINITIALIZED;
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = Enumerator(enumerator);
  if (FAILED(hr)) return hr;
  return enumerator->GetDevice(id.c_str(), &device);
}

std::vector<Endpoint> ListEndpoints() {
  ComApartment apartment;
  if (!apartment.ok) return {};
  std::vector<Endpoint> endpoints;
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(Enumerator(enumerator))) return endpoints;

  wchar_t *defaultRenderId = nullptr;
  wchar_t *defaultCaptureId = nullptr;
  ComPtr<IMMDevice> defaultRender;
  ComPtr<IMMDevice> defaultCapture;
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultRender))) {
    defaultRender->GetId(&defaultRenderId);
  }
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defaultCapture))) {
    defaultCapture->GetId(&defaultCaptureId);
  }

  const EDataFlow flows[] = {eRender, eCapture};
  for (EDataFlow flow : flows) {
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED | DEVICE_STATE_DISABLED, &collection))) {
      continue;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
      ComPtr<IMMDevice> device;
      if (FAILED(collection->Item(i, &device))) continue;
      wchar_t *id = nullptr;
      if (FAILED(device->GetId(&id)) || !id) continue;
      DWORD state = 0;
      device->GetState(&state);
      Endpoint endpoint;
      endpoint.id = id;
      endpoint.uid = WideToUtf8(id);
      endpoint.name = DeviceFriendlyName(device.Get());
      endpoint.input = flow == eCapture;
      endpoint.alive = state == DEVICE_STATE_ACTIVE;
      endpoint.isVirtual = IsVirtualDeviceName(endpoint.name);
      const wchar_t *def = flow == eRender ? defaultRenderId : defaultCaptureId;
      endpoint.isDefault = def && wcscmp(id, def) == 0;
      if (endpoint.alive) ReadMix(device.Get(), endpoint.sampleRate, endpoint.channels);
      CoTaskMemFree(id);
      endpoints.push_back(std::move(endpoint));
    }
  }
  CoTaskMemFree(defaultRenderId);
  CoTaskMemFree(defaultCaptureId);
  return endpoints;
}

bool HasVirtualOutput() {
  for (const Endpoint &endpoint : ListEndpoints()) {
    if (!endpoint.input && endpoint.alive && NameMatches(endpoint.name, kVirtualOutputPattern)) return true;
  }
  return false;
}

HRESULT OpenCapture(IMMDevice *device, DWORD extraFlags, int targetRate, WasapiClient &out) {
  if (!device) return E_POINTER;
  HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &out.client);
  if (FAILED(hr)) return hr;
  hr = InitClientFormat(out.client.Get(), extraFlags, targetRate, out);
  if (FAILED(hr)) {
    // A failed Initialize leaves the client unusable. Activate a fresh one
    // and take the mix format without the autoconvert flags.
    out.client.Reset();
    if (out.mix) {
      CoTaskMemFree(out.mix);
      out.mix = nullptr;
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &out.client);
    if (FAILED(hr)) return hr;
    hr = out.client->GetMixFormat(&out.mix);
    if (FAILED(hr)) return hr;
    hr = out.client->Initialize(AUDCLNT_SHAREMODE_SHARED, extraFlags, 200000, 0, out.mix, nullptr);
    if (FAILED(hr)) return hr;
    out.pcm16Mono = false;
    out.sampleRate = static_cast<int>(out.mix->nSamplesPerSec);
    out.channels = out.mix->nChannels;
  }
  return out.client->GetService(IID_PPV_ARGS(&out.capture));
}

HRESULT OpenProcessCapture(DWORD pid, int targetRate, WasapiClient &out) {
  ComApartment apartment;
  if (!apartment.ok) return CO_E_NOTINITIALIZED;
  auto *handler = new ActivateHandler();
  AUDIOCLIENT_ACTIVATION_PARAMS params{};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params.ProcessLoopbackParams.TargetProcessId = pid;
  params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

  PROPVARIANT activate{};
  PropVariantInit(&activate);
  activate.vt = VT_BLOB;
  activate.blob.cbSize = sizeof(params);
  activate.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

  IActivateAudioInterfaceAsyncOperation *operation = nullptr;
  HRESULT hr = ActivateAudioInterfaceAsync(
      VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activate, handler, &operation);
  if (FAILED(hr)) {
    handler->Release();
    return hr;
  }
  const DWORD wait = WaitForSingleObject(handler->done, 8000);
  if (operation) operation->Release();
  if (wait != WAIT_OBJECT_0) {
    handler->Release();
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }
  hr = handler->hr;
  ComPtr<IAudioClient> client = handler->client;
  handler->Release();
  if (FAILED(hr)) return hr;
  out.client = client;
  hr = InitClientFormat(out.client.Get(), AUDCLNT_STREAMFLAGS_LOOPBACK, targetRate, out);
  if (FAILED(hr)) return hr;
  return out.client->GetService(IID_PPV_ARGS(&out.capture));
}

HRESULT OpenRender(const std::string &pattern, int targetRate, WasapiClient &out, std::string &deviceName) {
  ComApartment apartment;
  if (!apartment.ok) return CO_E_NOTINITIALIZED;
  const std::string used = pattern.empty() ? kVirtualOutputPattern : pattern;
  ComPtr<IMMDevice> match;
  for (const Endpoint &endpoint : ListEndpoints()) {
    if (endpoint.input || !endpoint.alive) continue;
    if (!NameMatches(endpoint.name, used)) continue;
    ComPtr<IMMDevice> device;
    if (FAILED(DeviceFromId(endpoint.id, device))) continue;
    match = device;
    deviceName = endpoint.name;
    break;
  }
  if (!match) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

  HRESULT hr = match->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &out.client);
  if (FAILED(hr)) return hr;
  out.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  hr = InitClientFormat(out.client.Get(), AUDCLNT_STREAMFLAGS_EVENTCALLBACK, targetRate, out);
  if (FAILED(hr)) return hr;
  hr = out.client->SetEventHandle(out.event);
  if (FAILED(hr)) return hr;
  return out.client->GetService(IID_PPV_ARGS(&out.render));
}

HRESULT ReadCapture(WasapiClient &client, std::vector<int16_t> &out, uint32_t &framesOut) {
  out.clear();
  framesOut = 0;
  if (!client.capture) return E_POINTER;
  UINT32 packet = 0;
  HRESULT hr = client.capture->GetNextPacketSize(&packet);
  if (FAILED(hr)) return hr;
  if (packet == 0) return S_OK;
  BYTE *data = nullptr;
  UINT32 frames = 0;
  DWORD flags = 0;
  hr = client.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
  if (FAILED(hr)) return hr;
  const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
  if (silent) {
    out.assign(frames, 0);
  } else if (client.pcm16Mono) {
    out.resize(frames);
    std::memcpy(out.data(), data, frames * sizeof(int16_t));
  } else if (client.mix && IsFloatFormat(client.mix)) {
    DownmixFloatToMono(reinterpret_cast<const float *>(data), client.channels, frames, out);
  } else if (client.mix && IsPcm16Format(client.mix)) {
    DownmixToMono(reinterpret_cast<const int16_t *>(data), client.channels, frames, out);
  } else {
    out.assign(frames, 0);
  }
  client.capture->ReleaseBuffer(frames);
  framesOut = static_cast<uint32_t>(out.size());
  return S_OK;
}

uint32_t PushMonoS16(WasapiClient &client, const int16_t *samples, uint32_t frames) {
  if (!client.render || !client.client || frames == 0) return 0;
  UINT32 buffer = 0;
  UINT32 padding = 0;
  if (FAILED(client.client->GetBufferSize(&buffer)) || FAILED(client.client->GetCurrentPadding(&padding))) return 0;
  if (padding >= buffer) return 0;
  UINT32 available = buffer - padding;
  const UINT32 take = std::min(available, frames);
  BYTE *dest = nullptr;
  if (FAILED(client.render->GetBuffer(take, &dest)) || !dest) return 0;
  if (client.pcm16Mono) {
    std::memcpy(dest, samples, take * sizeof(int16_t));
  } else if (client.mix && IsFloatFormat(client.mix)) {
    auto *floats = reinterpret_cast<float *>(dest);
    const int channels = std::max(client.channels, 1);
    for (UINT32 i = 0; i < take; i++) {
      const float sample = std::clamp(samples[i] / 32768.0f, -1.0f, 1.0f);
      for (int c = 0; c < channels; c++) floats[i * channels + c] = sample;
    }
  } else if (client.mix && IsPcm16Format(client.mix)) {
    auto *pcm = reinterpret_cast<int16_t *>(dest);
    const int channels = std::max(client.channels, 1);
    for (UINT32 i = 0; i < take; i++) {
      for (int c = 0; c < channels; c++) pcm[i * channels + c] = samples[i];
    }
  }
  client.render->ReleaseBuffer(take, 0);
  return take;
}

}  // namespace babelvoice
