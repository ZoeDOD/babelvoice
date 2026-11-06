#pragma once

#include <node_api.h>

#include <cstdint>

namespace babelvoice {

napi_value PrepareTtsOutput(napi_env env, napi_callback_info info);
napi_value PlayPcm(napi_env env, napi_callback_info info);
napi_value FlushTtsOutput(napi_env env, napi_callback_info info);
napi_value GetTtsPendingMs(napi_env env, napi_callback_info info);
napi_value CloseTtsOutput(napi_env env, napi_callback_info info);

// 24 kHz mono int16 from the microphone tee. No-op until prepareTtsOutput.
void EnqueueMicPassthrough(const int16_t *samples, uint32_t frames);
void DisposeTtsOutput();

}  // namespace babelvoice
