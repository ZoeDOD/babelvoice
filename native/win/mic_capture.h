#pragma once

#include <node_api.h>

namespace babelvoice {

napi_value StartMicrophoneCapture(napi_env env, napi_callback_info info);
napi_value StopMicrophoneCapture(napi_env env, napi_callback_info info);
napi_value GetMicrophoneCaptureState(napi_env env, napi_callback_info info);
napi_value SetMicPassthroughEnabled(napi_env env, napi_callback_info info);
void StopMicrophoneForTeardown();

}  // namespace babelvoice
