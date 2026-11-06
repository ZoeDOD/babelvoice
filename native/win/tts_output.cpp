#include "tts_output.h"

#include "diagnostics.h"
#include "napi_util.h"
#include "resample.h"
#include "wasapi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace babelvoice {
namespace {

constexpr size_t kRingSeconds = 4;

struct Ring {
  std::vector<int16_t> data;
  size_t read = 0;
  size_t used = 0;
  std::mutex mutex;

  void Reset(int sampleRate) {
    std::lock_guard<std::mutex> lock(mutex);
    data.assign(static_cast<size_t>(sampleRate) * kRingSeconds, 0);
    read = 0;
    used = 0;
  }

  size_t Write(const int16_t *samples, size_t frames, uint64_t &dropped) {
    std::lock_guard<std::mutex> lock(mutex);
    if (data.empty()) return 0;
    const size_t room = data.size() - used;
    if (frames > room) {
      dropped += frames - room;
      frames = room;
    }
    for (size_t i = 0; i < frames; i++) {
      const size_t at = (read + used) % data.size();
      data[at] = samples[i];
      used++;
    }
    return frames;
  }

  size_t Read(int16_t *dest, size_t frames) {
    std::lock_guard<std::mutex> lock(mutex);
    const size_t take = std::min(frames, used);
    for (size_t i = 0; i < take; i++) {
      dest[i] = data[read];
      read = (read + 1) % data.size();
    }
    used -= take;
    if (take < frames) std::memset(dest + take, 0, (frames - take) * sizeof(int16_t));
    return take;
  }

  size_t Available() {
    std::lock_guard<std::mutex> lock(mutex);
    return used;
  }
};

struct Output {
  std::mutex mutex;
  Ring ring;
  WasapiClient client;
  std::thread worker;
  std::atomic<bool> running{false};
  int sampleRate = 0;
  std::string deviceName;
  std::unique_ptr<LinearResampler> passthroughResampler;
  std::unique_ptr<LinearResampler> playResampler;
  int playRate = 0;
  std::mutex startMutex;
  std::condition_variable startCv;
  bool startDone = false;
  std::string startError;
};

Output &GetOutput() {
  static Output output;
  return output;
}

void SignalTtsStart(const std::string &error) {
  Output &output = GetOutput();
  std::lock_guard<std::mutex> lock(output.startMutex);
  if (output.startDone) return;
  output.startDone = true;
  output.startError = error;
  output.startCv.notify_all();
}

// The audio client is created and used on this thread. WASAPI objects are
// apartment-bound, and the Electron main thread is an STA.
void AudioThread(std::string pattern, int requestedRate) {
  ComApartment apartment;
  Output &output = GetOutput();
  if (!apartment.ok) {
    SignalTtsStart("无法初始化 Windows 音频。");
    return;
  }
  std::string deviceName;
  const HRESULT hr = OpenRender(pattern, requestedRate, output.client, deviceName);
  if (FAILED(hr)) {
    output.client.Close();
    SignalTtsStart(hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
        ? "找不到虚拟声卡。请安装 VB-Audio Virtual Cable，安装后把会议软件的麦克风选为 CABLE Output。"
        : "打开虚拟声卡失败。");
    return;
  }
  output.sampleRate = output.client.pcm16Mono ? requestedRate : output.client.sampleRate;
  output.deviceName = deviceName;
  output.ring.Reset(output.sampleRate);
  if (FAILED(output.client.client->Start())) {
    output.client.Close();
    SignalTtsStart("虚拟声卡启动失败。");
    return;
  }
  output.running = true;
  SignalTtsStart("");
  SetRouteStatus(TtsDiagnostics(), "running");

  std::vector<int16_t> chunk(480);
  while (output.running.load()) {
    if (output.client.event) WaitForSingleObject(output.client.event, 20);
    if (!output.running.load()) break;
    UINT32 buffer = 0;
    UINT32 padding = 0;
    if (!output.client.client || FAILED(output.client.client->GetBufferSize(&buffer)) ||
        FAILED(output.client.client->GetCurrentPadding(&padding)) || padding >= buffer) {
      continue;
    }
    const UINT32 available = buffer - padding;
    if (available == 0) continue;
    if (chunk.size() < available) chunk.resize(available);
    const size_t got = output.ring.Read(chunk.data(), available);
    PushMonoS16(output.client, chunk.data(), available);
    if (got > 0) {
      UpdateDiagnostics(TtsDiagnostics(), chunk.data(), got * sizeof(int16_t), "running");
      TtsDiagnostics().queuedSamples = output.ring.Available();
    }
  }
  output.client.Close();
  output.sampleRate = 0;
  SetRouteStatus(TtsDiagnostics(), "idle");
}

void StopRender() {
  Output &output = GetOutput();
  output.running = false;
  if (output.client.event) SetEvent(output.client.event);
  if (output.worker.joinable()) output.worker.join();
}

}  // namespace

napi_value PrepareTtsOutput(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  double sampleRate = kDefaultCaptureSampleRate;
  if (!TtsSampleRateArg(env, args, argc, 0, sampleRate)) return nullptr;
  std::string pattern = kVirtualOutputPattern;
  if (argc >= 2) {
    napi_valuetype type;
    napi_typeof(env, args[1], &type);
    if (type == napi_string) pattern = StringArg(env, args[1]);
  }
  if (!HasVirtualOutput()) {
    Throw(env, "找不到虚拟声卡。请安装 VB-Audio Virtual Cable，安装后把会议软件的麦克风选为 CABLE Output。");
    return nullptr;
  }
  Output &output = GetOutput();
  StopRender();
  output.passthroughResampler.reset();
  output.playResampler.reset();
  output.playRate = 0;
  {
    std::lock_guard<std::mutex> lock(output.startMutex);
    output.startDone = false;
    output.startError.clear();
  }
  output.worker = std::thread(AudioThread, pattern, static_cast<int>(sampleRate));
  std::unique_lock<std::mutex> lock(output.startMutex);
  output.startCv.wait_for(lock, std::chrono::seconds(4), [&output] { return output.startDone; });
  if (!output.startError.empty() || !output.startDone) {
    const std::string message = output.startError.empty() ? "虚拟声卡启动超时。" : output.startError;
    lock.unlock();
    StopRender();
    SetLastError(TtsDiagnostics(), message.c_str());
    Throw(env, message.c_str());
    return nullptr;
  }
  return MakeBool(env, true);
}

napi_value PlayPcm(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    Throw(env, "playPcm requires a Buffer");
    return nullptr;
  }
  bool isBuffer = false;
  napi_is_buffer(env, args[0], &isBuffer);
  if (!isBuffer) {
    Throw(env, "playPcm requires a Buffer");
    return nullptr;
  }
  void *bytes = nullptr;
  size_t length = 0;
  napi_get_buffer_info(env, args[0], &bytes, &length);
  if (!bytes || length < 2 || (length % 2) != 0) return MakeBool(env, true);
  double sampleRate = kDefaultCaptureSampleRate;
  if (!TtsSampleRateArg(env, args, argc, 1, sampleRate)) return nullptr;

  Output &output = GetOutput();
  std::lock_guard<std::mutex> lock(output.mutex);
  if (!output.running.load() || output.sampleRate == 0) {
    Throw(env, "virtual loopback output device not found");
    return nullptr;
  }
  const auto *samples = static_cast<const int16_t *>(bytes);
  const uint32_t frames = static_cast<uint32_t>(length / 2);
  std::vector<int16_t> converted;
  const int16_t *out = samples;
  uint32_t outFrames = frames;
  if (static_cast<int>(sampleRate) != output.sampleRate) {
    if (!output.playResampler || output.playRate != static_cast<int>(sampleRate)) {
      output.playResampler = std::make_unique<LinearResampler>(sampleRate, output.sampleRate);
      output.playRate = static_cast<int>(sampleRate);
    }
    output.playResampler->Process(samples, frames, converted);
    out = converted.data();
    outFrames = static_cast<uint32_t>(converted.size());
  }
  uint64_t dropped = 0;
  output.ring.Write(out, outFrames, dropped);
  if (dropped) TtsDiagnostics().droppedSamples += dropped;
  TtsDiagnostics().queuedSamples = output.ring.Available();
  return MakeBool(env, true);
}

napi_value FlushTtsOutput(napi_env env, napi_callback_info info) {
  (void)info;
  Output &output = GetOutput();
  const int rate = output.sampleRate > 0 ? output.sampleRate : 16000;
  for (int i = 0; i < 200 && output.ring.Available() > static_cast<size_t>(rate / 50); i++) Sleep(10);
  return MakeBool(env, true);
}

napi_value GetTtsPendingMs(napi_env env, napi_callback_info info) {
  (void)info;
  Output &output = GetOutput();
  const int rate = output.sampleRate > 0 ? output.sampleRate : 1;
  const double ms = output.ring.Available() * 1000.0 / rate;
  return MakeNumber(env, ms);
}

napi_value CloseTtsOutput(napi_env env, napi_callback_info info) {
  (void)info;
  DisposeTtsOutput();
  return MakeBool(env, true);
}

void EnqueueMicPassthrough(const int16_t *samples, uint32_t frames) {
  Output &output = GetOutput();
  std::lock_guard<std::mutex> lock(output.mutex);
  if (!output.running.load() || !samples || frames == 0 || output.sampleRate == 0) return;
  const int16_t *out = samples;
  uint32_t outFrames = frames;
  std::vector<int16_t> converted;
  if (output.sampleRate != 24000) {
    if (!output.passthroughResampler) {
      output.passthroughResampler = std::make_unique<LinearResampler>(24000, output.sampleRate);
    }
    output.passthroughResampler->Process(samples, frames, converted);
    out = converted.data();
    outFrames = static_cast<uint32_t>(converted.size());
  }
  uint64_t dropped = 0;
  output.ring.Write(out, outFrames, dropped);
  if (dropped) TtsDiagnostics().droppedSamples += dropped;
}

void DisposeTtsOutput() {
  Output &output = GetOutput();
  std::lock_guard<std::mutex> lock(output.mutex);
  StopRender();
  output.client.Close();
  output.sampleRate = 0;
  output.passthroughResampler.reset();
  output.playResampler.reset();
  output.playRate = 0;
  SetRouteStatus(TtsDiagnostics(), "idle");
}

}  // namespace babelvoice
