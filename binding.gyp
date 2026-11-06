{
  "targets": [
    {
      "target_name": "babelvoice_native",
      "sources": [
        "native/napi_util.cc",
        "native/diagnostics.cc",
        "native/capture_sink.cc",
        "native/promise_settler.cc"
      ],
      "defines": [
        # Pin the Node-API surface. 4 is the lowest version that has every
        # napi_* call the addon makes (threadsafe functions are 4,
        # napi_add_env_cleanup_hook is 3, the rest is 1); anything newer
        # fails to compile instead of silently raising the runtime floor.
        "NAPI_VERSION=4"
      ],
      "conditions": [
        [
          "OS==\"mac\"",
          {
            "sources": [
              "native/macos_audio_bridge.mm",
              "native/devices.mm",
              "native/pcm_resampler.cc",
              "native/tts_output.mm",
              "native/mic_capture.mm",
              "native/sck_capture.mm",
              "native/process_tap.mm"
            ],
            "xcode_settings": {
              "CLANG_ENABLE_OBJC_ARC": "YES",
              "MACOSX_DEPLOYMENT_TARGET": "14.0",
              "OTHER_CPLUSPLUSFLAGS": ["-std=c++17"],
              # -fvisibility=hidden: the subsystems now share symbols across
              # translation units, but the only exports stay the module
              # registration ones (NAPI_MODULE_EXPORT is visibility=default).
              "GCC_SYMBOLS_PRIVATE_EXTERN": "YES",
              "OTHER_LDFLAGS": [
                "-framework",
                "Foundation",
                "-framework",
                "ScreenCaptureKit",
                "-framework",
                "CoreMedia",
                "-framework",
                "CoreAudio",
                "-framework",
                "AudioToolbox",
                "-framework",
                "AppKit",
                "-framework",
                "AVFoundation",
                "-framework",
                "ApplicationServices"
              ]
            }
          }
        ],
        [
          "OS==\"win\"",
          {
            "sources": [
              "native/win/wasapi.cpp",
              "native/win/mic_capture.cpp",
              "native/win/listen_capture.cpp",
              "native/win/tts_output.cpp",
              "native/win/audio_bridge.cpp"
            ],
            "include_dirs": ["native"],
            "defines": [
              "UNICODE",
              "_UNICODE",
              "WIN32_LEAN_AND_MEAN",
              "NOMINMAX",
              "NTDDI_VERSION=0x0A00000C",
              "_WIN32_WINNT=0x0A00"
            ],
            "libraries": [
              "-lole32.lib",
              "-loleaut32.lib",
              "-luuid.lib",
              "-lavrt.lib",
              "-lmmdevapi.lib"
            ],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "AdditionalOptions": ["/std:c++17", "/EHsc", "/utf-8"]
              }
            }
          }
        ]
      ]
    }
  ]
}
