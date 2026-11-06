#include <node_api.h>

#include <cstring>

#include "diagnostics.h"
#include "listen_capture.h"
#include "mic_capture.h"
#include "napi_util.h"
#include "tts_output.h"
#include "wasapi.h"

namespace babelvoice {
namespace {

napi_value ListAudioDevices(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value array;
  napi_create_array(env, &array);
  uint32_t index = 0;
  for (const Endpoint &endpoint : ListEndpoints()) {
    napi_value obj;
    napi_create_object(env, &obj);
    SetNamed(env, obj, "id", MakeNumber(env, index));
    SetNamed(env, obj, "uid", MakeString(env, endpoint.uid.c_str()));
    SetNamed(env, obj, "name", MakeString(env, endpoint.name.c_str()));
    SetNamed(env, obj, "kind", MakeString(env, endpoint.input ? "input" : "output"));
    SetNamed(env, obj, "isDefault", MakeBool(env, endpoint.isDefault));
    SetNamed(env, obj, "isVirtual", MakeBool(env, endpoint.isVirtual));
    SetNamed(env, obj, "isAlive", MakeBool(env, endpoint.alive));
    SetNamed(env, obj, "sampleRate", MakeNumber(env, endpoint.sampleRate));
    SetNamed(env, obj, "channels", MakeNumber(env, endpoint.channels));
    napi_set_element(env, array, index++, obj);
  }
  return array;
}

napi_value GetCapabilities(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value obj;
  napi_create_object(env, &obj);
  SetNamed(env, obj, "platform", MakeString(env, "win32"));
  SetNamed(env, obj, "systemAudioCapture", MakeBool(env, true));
  SetNamed(env, obj, "nativeTtsPlayback", MakeBool(env, true));
  SetNamed(env, obj, "processAudioCapture", MakeBool(env, true));
  SetNamed(env, obj, "microphoneCapture", MakeBool(env, true));
  SetNamed(env, obj, "audioDevices", MakeBool(env, true));
  SetNamed(env, obj, "permissions", MakeBool(env, true));
  SetNamed(env, obj, "windowBehavior", MakeBool(env, true));
  SetNamed(env, obj, "hasVirtualOutput", MakeBool(env, HasVirtualOutput()));
  return obj;
}

napi_value GetPermissions(napi_env env, napi_callback_info info) {
  (void)info;
  napi_value obj;
  napi_create_object(env, &obj);
  // Loopback capture does not use the screen-recording consent macOS requires.
  SetNamed(env, obj, "screen", MakeString(env, "granted"));
  SetNamed(env, obj, "microphone", MakeString(env, MicConsent()));
  SetNamed(env, obj, "inputMonitoring", MakeString(env, "granted"));
  SetNamed(env, obj, "accessibility", MakeString(env, "granted"));
  return obj;
}

napi_value ConfigureSubtitleWindow(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    Throw(env, "configureSubtitleWindow requires a native window handle buffer");
    return nullptr;
  }
  bool isBuffer = false;
  napi_is_buffer(env, args[0], &isBuffer);
  if (!isBuffer) {
    Throw(env, "native window handle must be a Buffer");
    return nullptr;
  }
  void *handleBytes = nullptr;
  size_t handleLength = 0;
  napi_get_buffer_info(env, args[0], &handleBytes, &handleLength);
  if (!handleBytes || handleLength < sizeof(HWND)) {
    Throw(env, "native window handle buffer is invalid");
    return nullptr;
  }
  HWND hwnd = *reinterpret_cast<HWND *>(handleBytes);
  if (!hwnd || !IsWindow(hwnd)) return MakeBool(env, true);
  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  return MakeBool(env, true);
}

void CleanupNativeBridge(void *) {
  StopMicrophoneForTeardown();
  StopSystemAudioForTeardown();
  StopProcessAudioForTeardown();
  DisposeTtsOutput();
}

napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor descriptors[] = {
      {"getCapabilities", nullptr, GetCapabilities, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getPermissions", nullptr, GetPermissions, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"listAudioDevices", nullptr, ListAudioDevices, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getAudioDiagnostics", nullptr, GetAudioDiagnostics, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"startMicrophoneCapture", nullptr, StartMicrophoneCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stopMicrophoneCapture", nullptr, StopMicrophoneCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"startSystemAudioCapture", nullptr, StartSystemAudioCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stopSystemAudioCapture", nullptr, StopSystemAudioCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"listAudioCapableApplications", nullptr, ListAudioCapableApplications, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"startProcessAudioCapture", nullptr, StartProcessAudioCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stopProcessAudioCapture", nullptr, StopProcessAudioCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"prepareTtsOutput", nullptr, PrepareTtsOutput, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"playPcm", nullptr, PlayPcm, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"flushTtsOutput", nullptr, FlushTtsOutput, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getTtsPendingMs", nullptr, GetTtsPendingMs, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"closeTtsOutput", nullptr, CloseTtsOutput, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setMicPassthroughEnabled", nullptr, SetMicPassthroughEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getMicrophoneCaptureState", nullptr, GetMicrophoneCaptureState, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"configureSubtitleWindow", nullptr, ConfigureSubtitleWindow, nullptr, nullptr, nullptr, napi_default, nullptr}};
  napi_define_properties(env, exports, sizeof(descriptors) / sizeof(descriptors[0]), descriptors);
  napi_add_env_cleanup_hook(env, CleanupNativeBridge, nullptr);
  return exports;
}

}  // namespace
}  // namespace babelvoice

NAPI_MODULE(NODE_GYP_MODULE_NAME, babelvoice::Init)
