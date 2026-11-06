#pragma once

#include <node_api.h>

namespace babelvoice {

napi_value StartSystemAudioCapture(napi_env env, napi_callback_info info);
napi_value StopSystemAudioCapture(napi_env env, napi_callback_info info);
napi_value ListAudioCapableApplications(napi_env env, napi_callback_info info);
napi_value StartProcessAudioCapture(napi_env env, napi_callback_info info);
napi_value StopProcessAudioCapture(napi_env env, napi_callback_info info);
void StopSystemAudioForTeardown();
void StopProcessAudioForTeardown();

}  // namespace babelvoice
