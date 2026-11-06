#include "listen_capture.h"

#include "capture_sink.h"
#include "diagnostics.h"
#include "napi_util.h"
#include "promise_settler.h"
#include "resample.h"
#include "wasapi.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace babelvoice {
namespace {

struct AppProcess {
  DWORD pid = 0;
  std::string bundleId;
  std::string name;
};

struct ListenRun {
  std::shared_ptr<CaptureSink> sink;
  std::atomic<bool> stop{false};
  std::atomic<bool> alive{false};
  std::thread worker;
  std::mutex mutex;
};

ListenRun &SystemRun() {
  static ListenRun run;
  return run;
}

ListenRun &ProcessRun() {
  static ListenRun run;
  return run;
}

void Pump(WasapiClient &client, ListenRun &run, int targetRate) {
  LinearResampler resampler(client.sampleRate, targetRate);
  if (FAILED(client.client->Start())) {
    run.sink->PostError("音频流启动失败。");
    return;
  }
  SetRouteStatus(ListenDiagnostics(), "running");
  std::vector<int16_t> raw;
  std::vector<int16_t> converted;
  while (!run.stop.load()) {
    uint32_t frames = 0;
    const HRESULT hr = ReadCapture(client, raw, frames);
    if (FAILED(hr)) {
      if (!run.stop.load()) run.sink->PostError(HresultText("收听中断", hr).c_str());
      break;
    }
    if (frames == 0) {
      Sleep(5);
      continue;
    }
    const int16_t *samples = raw.data();
    uint32_t count = frames;
    if (client.sampleRate != targetRate) {
      resampler.Process(raw.data(), frames, converted);
      samples = converted.data();
      count = static_cast<uint32_t>(converted.size());
    }
    if (count == 0) continue;
    UpdateDiagnostics(ListenDiagnostics(), samples, count * sizeof(int16_t), "running");
    run.sink->PostPcm(CopyPacket(samples, count * sizeof(int16_t)));
  }
  client.client->Stop();
}

void StopRun(ListenRun &run, bool detachOnly) {
  run.stop = true;
  if (run.worker.joinable()) run.worker.join();
  run.alive = false;
  if (run.sink) {
    if (detachOnly) run.sink->Detach();
    else {
      run.sink->BeginStop();
      run.sink->Release();
    }
    run.sink.reset();
  }
  SetRouteStatus(ListenDiagnostics(), "idle");
}

BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param) {
  auto *apps = reinterpret_cast<std::vector<AppProcess> *>(param);
  if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
  if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;
  for (const AppProcess &existing : *apps) {
    if (existing.pid == pid) return TRUE;
  }
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return TRUE;
  wchar_t path[MAX_PATH] = {};
  DWORD size = MAX_PATH;
  const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
  CloseHandle(process);
  if (!ok) return TRUE;
  wchar_t title[256] = {};
  GetWindowTextW(hwnd, title, 256);
  AppProcess app;
  app.pid = pid;
  app.bundleId = WideToUtf8(path);
  app.name = title[0] ? WideToUtf8(title) : app.bundleId;
  const size_t slash = app.bundleId.find_last_of("\\/");
  if (app.name == app.bundleId && slash != std::string::npos) app.name = app.bundleId.substr(slash + 1);
  apps->push_back(std::move(app));
  return TRUE;
}

std::vector<AppProcess> ListApps() {
  std::vector<AppProcess> apps;
  EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&apps));
  return apps;
}

DWORD FindPid(const std::string &bundleId) {
  if (bundleId.empty()) return 0;
  for (const AppProcess &app : ListApps()) {
    if (_stricmp(app.bundleId.c_str(), bundleId.c_str()) == 0) return app.pid;
    const size_t slash = app.bundleId.find_last_of("\\/");
    const std::string file = slash == std::string::npos ? app.bundleId : app.bundleId.substr(slash + 1);
    if (_stricmp(file.c_str(), bundleId.c_str()) == 0) return app.pid;
  }
  return 0;
}

napi_value BeginListen(
    napi_env env,
    napi_callback_info info,
    ListenRun &run,
    const char *busyMessage,
    size_t callbackIndex,
    size_t errorIndex,
    size_t rateIndex,
    const std::function<HRESULT(int, WasapiClient &)> &open) {
  if (run.alive.load()) {
    Throw(env, busyMessage);
    return nullptr;
  }
  if (run.worker.joinable()) run.worker.join();
  if (run.sink) {
    run.sink->Release();
    run.sink.reset();
  }
  size_t argc = 4;
  napi_value args[4];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc <= callbackIndex) {
    Throw(env, "capture requires a callback");
    return nullptr;
  }
  napi_valuetype type;
  napi_typeof(env, args[callbackIndex], &type);
  if (type != napi_function) {
    Throw(env, "capture callback must be a function");
    return nullptr;
  }
  napi_value onError = OptionalFunctionArg(env, args, argc, errorIndex);
  double sampleRate = kDefaultCaptureSampleRate;
  if (!CaptureSampleRateArg(env, args, argc, rateIndex, sampleRate)) return nullptr;

  run.sink = CreateCaptureSink(env, args[callbackIndex], onError, "BabelVoiceListenCapture", nullptr);
  if (!run.sink) {
    Throw(env, "failed to create capture callback");
    return nullptr;
  }
  napi_value promise = nullptr;
  auto settler = CreateSettler(env, "BabelVoiceListenStart", &promise);
  if (!settler) {
    run.sink->Release();
    run.sink.reset();
    Throw(env, "failed to create capture promise");
    return nullptr;
  }
  run.stop = false;
  run.alive = true;
  const int rate = static_cast<int>(sampleRate);
  SetRouteStatus(ListenDiagnostics(), "starting");
  run.worker = std::thread([open, rate, settler, &run] {
    ComApartment apartment;
    if (!apartment.ok) {
      Settle(settler, false, "无法初始化 Windows 音频。");
      run.alive = false;
      return;
    }
    WasapiClient client;
    const HRESULT hr = open(rate, client);
    if (FAILED(hr) || run.stop.load()) {
      Settle(settler, false, HresultText("收听启动失败", FAILED(hr) ? hr : E_ABORT));
      run.alive = false;
      return;
    }
    Settle(settler, true, "");
    Pump(client, run, rate);
    run.alive = false;
  });
  return promise;
}

}  // namespace

napi_value StartSystemAudioCapture(napi_env env, napi_callback_info info) {
  return BeginListen(env, info, SystemRun(), "system audio capture is already running", 0, 1, 2, [](int rate, WasapiClient &client) {
    ComPtr<IMMDevice> device;
    HRESULT hr = DefaultDevice(eRender, device);
    if (FAILED(hr)) return hr;
    return OpenCapture(device.Get(), AUDCLNT_STREAMFLAGS_LOOPBACK, rate, client);
  });
}

napi_value StopSystemAudioCapture(napi_env env, napi_callback_info info) {
  (void)info;
  StopRun(SystemRun(), false);
  napi_value promise;
  napi_deferred deferred;
  napi_create_promise(env, &deferred, &promise);
  napi_resolve_deferred(env, deferred, MakeBool(env, true));
  return promise;
}

napi_value ListAudioCapableApplications(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value array;
  napi_create_array(env, &array);
  uint32_t index = 0;
  for (const AppProcess &app : ListApps()) {
    napi_value entry;
    napi_create_object(env, &entry);
    SetNamed(env, entry, "pid", MakeNumber(env, app.pid));
    SetNamed(env, entry, "bundleId", MakeString(env, app.bundleId.c_str()));
    SetNamed(env, entry, "name", MakeString(env, app.name.c_str()));
    napi_set_element(env, array, index++, entry);
  }
  return array;
}

napi_value StartProcessAudioCapture(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    Throw(env, "startProcessAudioCapture requires a bundle id");
    return nullptr;
  }
  const std::string bundleId = StringArg(env, args[0]);
  const DWORD pid = FindPid(bundleId);
  if (pid == 0) {
    Throw(env, "找不到要收听的应用。请确认它已经打开，然后重新选择。");
    return nullptr;
  }
  return BeginListen(env, info, ProcessRun(), "process audio capture is already running", 1, 2, 3, [pid](int rate, WasapiClient &client) {
    return OpenProcessCapture(pid, rate, client);
  });
}

napi_value StopProcessAudioCapture(napi_env env, napi_callback_info info) {
  (void)info;
  StopRun(ProcessRun(), false);
  napi_value promise;
  napi_deferred deferred;
  napi_create_promise(env, &deferred, &promise);
  napi_resolve_deferred(env, deferred, MakeBool(env, true));
  return promise;
}

void StopSystemAudioForTeardown() { StopRun(SystemRun(), true); }
void StopProcessAudioForTeardown() { StopRun(ProcessRun(), true); }

}  // namespace babelvoice
