#include "mic_capture.h"

#include "capture_sink.h"
#include "diagnostics.h"
#include "napi_util.h"
#include "resample.h"
#include "tts_output.h"
#include "wasapi.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace babelvoice {
namespace {

constexpr int kHubRate = 24000;

struct Consumer {
  int64_t id = 0;
  bool deliver = false;
  double sampleRate = kDefaultCaptureSampleRate;
  std::shared_ptr<CaptureSink> sink;
  std::unique_ptr<LinearResampler> resampler;
};

struct Hub {
  std::mutex mutex;
  std::vector<std::shared_ptr<Consumer>> consumers;
  int64_t nextId = 1;
  std::atomic<bool> running{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> passthrough{false};
  std::string deviceUid;
  std::thread worker;
  int queueStarts = 0;
  std::mutex startMutex;
  std::condition_variable startCv;
  bool startDone = false;
  std::string startError;
};

Hub &GetHub() {
  static Hub hub;
  return hub;
}

void SignalStart(const std::string &error) {
  Hub &hub = GetHub();
  std::lock_guard<std::mutex> lock(hub.startMutex);
  if (hub.startDone) return;
  hub.startDone = true;
  hub.startError = error;
  hub.startCv.notify_all();
}

void FailConsumers(const char *message) {
  Hub &hub = GetHub();
  {
    std::lock_guard<std::mutex> lock(hub.mutex);
    hub.failed = true;
    for (const auto &consumer : hub.consumers) {
      if (consumer->sink) consumer->sink->PostError(message);
    }
  }
  SetLastError(MicDiagnostics(), message);
  SetRouteStatus(MicDiagnostics(), "failed");
  SignalStart(message);
}

std::string VirtualMicRefusal(const std::string &uid) {
  ComPtr<IMMDevice> device;
  HRESULT hr = uid.empty() ? DefaultDevice(eCapture, device) : DeviceFromId(Utf8ToWide(uid), device);
  if (FAILED(hr) || !device) return uid.empty() ? "找不到默认麦克风。" : "找不到所选麦克风。";
  const std::string name = DeviceFriendlyName(device.Get());
  if (IsVirtualDeviceName(name)) {
    return "不能把虚拟声卡（" + name + "）当作麦克风，否则译音会被再次录入。请改选真实麦克风。";
  }
  return {};
}

void CaptureLoop(std::string uid) {
  ComApartment apartment;
  if (!apartment.ok) {
    FailConsumers("无法初始化 Windows 音频。");
    return;
  }
  ComPtr<IMMDevice> device;
  HRESULT hr = uid.empty() ? DefaultDevice(eCapture, device) : DeviceFromId(Utf8ToWide(uid), device);
  if (FAILED(hr) || !device) {
    FailConsumers(uid.empty() ? "找不到默认麦克风。" : "找不到所选麦克风。");
    return;
  }
  WasapiClient client;
  hr = OpenCapture(device.Get(), 0, kHubRate, client);
  if (FAILED(hr)) {
    FailConsumers(HresultText("打开麦克风失败", hr).c_str());
    return;
  }
  hr = client.client->Start();
  if (FAILED(hr)) {
    FailConsumers(HresultText("麦克风启动失败", hr).c_str());
    return;
  }
  SetRouteStatus(MicDiagnostics(), "running");
  SignalStart("");
  LinearResampler toHub(client.sampleRate, kHubRate);
  std::vector<int16_t> raw;
  std::vector<int16_t> hubRate;
  Hub &hub = GetHub();
  while (hub.running.load()) {
    uint32_t frames = 0;
    hr = ReadCapture(client, raw, frames);
    if (FAILED(hr)) {
      FailConsumers(HresultText("麦克风中断", hr).c_str());
      break;
    }
    if (frames == 0) {
      Sleep(5);
      continue;
    }
    const int16_t *samples = raw.data();
    uint32_t count = frames;
    if (client.sampleRate != kHubRate) {
      toHub.Process(raw.data(), frames, hubRate);
      samples = hubRate.data();
      count = static_cast<uint32_t>(hubRate.size());
    }
    if (count == 0) continue;
    UpdateDiagnostics(MicDiagnostics(), samples, count * sizeof(int16_t), "running");
    if (hub.passthrough.load()) EnqueueMicPassthrough(samples, count);

    std::vector<std::shared_ptr<Consumer>> consumers;
    {
      std::lock_guard<std::mutex> lock(hub.mutex);
      consumers = hub.consumers;
    }
    for (const auto &consumer : consumers) {
      if (!consumer->deliver || !consumer->sink) continue;
      const int16_t *out = samples;
      uint32_t outCount = count;
      std::vector<int16_t> converted;
      if (consumer->resampler) {
        consumer->resampler->Process(samples, count, converted);
        out = converted.data();
        outCount = static_cast<uint32_t>(converted.size());
      }
      if (outCount == 0) continue;
      consumer->sink->PostPcm(CopyPacket(out, outCount * sizeof(int16_t)));
    }
  }
  client.client->Stop();
}

void StopWorker() {
  Hub &hub = GetHub();
  hub.running = false;
  if (hub.worker.joinable()) hub.worker.join();
  SetRouteStatus(MicDiagnostics(), "idle");
}

void StartWorker(const std::string &uid) {
  Hub &hub = GetHub();
  if (hub.worker.joinable() && hub.deviceUid == uid && !hub.failed.load()) return;
  StopWorker();
  hub.failed = false;
  hub.deviceUid = uid;
  hub.running = true;
  hub.queueStarts++;
  {
    std::lock_guard<std::mutex> lock(hub.startMutex);
    hub.startDone = false;
    hub.startError.clear();
  }
  hub.worker = std::thread(CaptureLoop, uid);
  std::unique_lock<std::mutex> lock(hub.startMutex);
  hub.startCv.wait_for(lock, std::chrono::seconds(3), [&hub] { return hub.startDone; });
  if (!hub.startError.empty() || !hub.startDone) {
    const std::string message = hub.startError.empty() ? "麦克风启动超时。" : hub.startError;
    lock.unlock();
    StopWorker();
    throw std::runtime_error(message);
  }
}

napi_value FailStart(napi_env env, const char *message) {
  SetLastError(MicDiagnostics(), message);
  Throw(env, message);
  return nullptr;
}

}  // namespace

napi_value StartMicrophoneCapture(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  napi_value onPcm = OptionalFunctionArg(env, args, argc, 0);
  if (!onPcm && argc >= 1) {
    napi_valuetype type;
    napi_typeof(env, args[0], &type);
    if (type != napi_null && type != napi_undefined) {
      Throw(env, "startMicrophoneCapture callback must be a function or null");
      return nullptr;
    }
  }
  std::string deviceUid;
  if (argc >= 2) {
    napi_valuetype uidType;
    napi_typeof(env, args[1], &uidType);
    if (uidType == napi_string) deviceUid = StringArg(env, args[1]);
  }
  napi_value onError = OptionalFunctionArg(env, args, argc, 2);
  double sampleRate = kDefaultCaptureSampleRate;
  if (!CaptureSampleRateArg(env, args, argc, 3, sampleRate)) return nullptr;

  if (std::strcmp(MicConsent(), "denied") == 0) {
    return FailStart(env, "麦克风权限已被拒绝。请到「设置 → 隐私和安全性 → 麦克风」允许 BabelVoice，然后重试。");
  }
  const std::string refusal = VirtualMicRefusal(deviceUid);
  if (!refusal.empty()) return FailStart(env, refusal.c_str());

  auto consumer = std::make_shared<Consumer>();
  consumer->deliver = onPcm != nullptr;
  consumer->sampleRate = sampleRate;
  if (consumer->deliver && sampleRate != kHubRate) {
    consumer->resampler = std::make_unique<LinearResampler>(kHubRate, sampleRate);
  }
  consumer->sink = CreateCaptureSink(env, onPcm, onError, "BabelVoiceMicrophoneCapture", nullptr);
  if (!consumer->sink) {
    Throw(env, "failed to create microphone callback");
    return nullptr;
  }

  Hub &hub = GetHub();
  {
    std::lock_guard<std::mutex> lock(hub.mutex);
    consumer->id = hub.nextId++;
    hub.consumers.push_back(consumer);
  }
  try {
    StartWorker(deviceUid);
  } catch (const std::exception &err) {
    std::lock_guard<std::mutex> lock(hub.mutex);
    hub.consumers.pop_back();
    consumer->sink->Release();
    return FailStart(env, err.what());
  }
  if (hub.failed.load()) {
    std::string message = "麦克风启动失败。";
    std::lock_guard<std::mutex> lock(hub.mutex);
    hub.consumers.pop_back();
    consumer->sink->Release();
    return FailStart(env, message.c_str());
  }
  return MakeNumber(env, static_cast<double>(consumer->id));
}

napi_value StopMicrophoneCapture(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  Hub &hub = GetHub();
  bool stopAll = true;
  double id = 0;
  if (argc >= 1) {
    napi_valuetype type = napi_undefined;
    napi_typeof(env, args[0], &type);
    if (type == napi_number) {
      napi_get_value_double(env, args[0], &id);
      stopAll = false;
    }
  }
  std::vector<std::shared_ptr<Consumer>> dropped;
  {
    std::lock_guard<std::mutex> lock(hub.mutex);
    if (stopAll) {
      dropped.swap(hub.consumers);
    } else {
      for (auto it = hub.consumers.begin(); it != hub.consumers.end();) {
        if ((*it)->id == static_cast<int64_t>(id)) {
          dropped.push_back(*it);
          it = hub.consumers.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  for (const auto &consumer : dropped) {
    if (consumer->sink) {
      consumer->sink->BeginStop();
      consumer->sink->Release();
    }
  }
  bool empty = false;
  {
    std::lock_guard<std::mutex> lock(hub.mutex);
    empty = hub.consumers.empty();
  }
  if (empty) StopWorker();
  return MakeBool(env, true);
}

napi_value GetMicrophoneCaptureState(napi_env env, napi_callback_info info) {
  (void)info;
  Hub &hub = GetHub();
  std::lock_guard<std::mutex> lock(hub.mutex);
  int pcm = 0;
  for (const auto &consumer : hub.consumers) {
    if (consumer->deliver) pcm++;
  }
  napi_value obj;
  napi_create_object(env, &obj);
  SetNamed(env, obj, "running", MakeBool(env, hub.running.load()));
  SetNamed(env, obj, "sampleRate", MakeNumber(env, hub.running.load() ? kHubRate : 0));
  SetNamed(env, obj, "deviceUid", MakeString(env, hub.deviceUid.c_str()));
  SetNamed(env, obj, "failed", MakeBool(env, hub.failed.load()));
  SetNamed(env, obj, "consumers", MakeNumber(env, static_cast<double>(hub.consumers.size())));
  SetNamed(env, obj, "pcmConsumers", MakeNumber(env, pcm));
  SetNamed(env, obj, "queueStarts", MakeNumber(env, hub.queueStarts));
  SetNamed(env, obj, "passthrough", MakeBool(env, hub.passthrough.load()));
  return obj;
}

napi_value SetMicPassthroughEnabled(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  bool enabled = false;
  if (argc >= 1) napi_get_value_bool(env, args[0], &enabled);
  GetHub().passthrough = enabled;
  return MakeBool(env, true);
}

void StopMicrophoneForTeardown() {
  Hub &hub = GetHub();
  hub.running = false;
  hub.passthrough = false;
  if (hub.worker.joinable()) hub.worker.join();
  std::lock_guard<std::mutex> lock(hub.mutex);
  for (const auto &consumer : hub.consumers) {
    if (consumer->sink) consumer->sink->Detach();
  }
  hub.consumers.clear();
}

}  // namespace babelvoice
