{
  "variables": {
    "tree_sitter_root": "<!(node script/provision-native-deps.js --print-tree-sitter-root)",
    "wasmtime_root": "<!(node script/provision-native-deps.js --print-wasmtime-root)",
    "pcre2_root": "<!(node script/provision-native-deps.js --print-pcre2-root)"
  },
  "targets": [
    {
      "target_name": "pcre2-runtime",
      "type": "static_library",
      "sources": [
        "<(pcre2_root)/src/pcre2_chartables.c",
        "<(pcre2_root)/src/pcre2_auto_possess.c",
        "<(pcre2_root)/src/pcre2_chkdint.c",
        "<(pcre2_root)/src/pcre2_compile.c",
        "<(pcre2_root)/src/pcre2_config.c",
        "<(pcre2_root)/src/pcre2_context.c",
        "<(pcre2_root)/src/pcre2_convert.c",
        "<(pcre2_root)/src/pcre2_dfa_match.c",
        "<(pcre2_root)/src/pcre2_error.c",
        "<(pcre2_root)/src/pcre2_extuni.c",
        "<(pcre2_root)/src/pcre2_find_bracket.c",
        "<(pcre2_root)/src/pcre2_maketables.c",
        "<(pcre2_root)/src/pcre2_match.c",
        "<(pcre2_root)/src/pcre2_match_data.c",
        "<(pcre2_root)/src/pcre2_newline.c",
        "<(pcre2_root)/src/pcre2_ord2utf.c",
        "<(pcre2_root)/src/pcre2_pattern_info.c",
        "<(pcre2_root)/src/pcre2_script_run.c",
        "<(pcre2_root)/src/pcre2_serialize.c",
        "<(pcre2_root)/src/pcre2_string_utils.c",
        "<(pcre2_root)/src/pcre2_study.c",
        "<(pcre2_root)/src/pcre2_substitute.c",
        "<(pcre2_root)/src/pcre2_substring.c",
        "<(pcre2_root)/src/pcre2_tables.c",
        "<(pcre2_root)/src/pcre2_ucd.c",
        "<(pcre2_root)/src/pcre2_valid_utf.c",
        "<(pcre2_root)/src/pcre2_xclass.c"
      ],
      "include_dirs": ["<(pcre2_root)/src"],
      "defines": ["HAVE_CONFIG_H", "PCRE2_CODE_UNIT_WIDTH=16", "PCRE2_STATIC"],
      "direct_dependent_settings": {
        "include_dirs": ["<(pcre2_root)/src"],
        "defines": ["PCRE2_CODE_UNIT_WIDTH=16", "PCRE2_STATIC"]
      }
    },
    {
      "target_name": "tree-sitter-runtime",
      "type": "static_library",
      "sources": [
        "<(tree_sitter_root)/lib/src/lib.c"
      ],
      "include_dirs": [
        "<(tree_sitter_root)/lib/include",
        "<(tree_sitter_root)/lib/src",
        "<(tree_sitter_root)/lib/src/wasm",
        "<(wasmtime_root)/include"
      ],
      "defines": [
        "TREE_SITTER_FEATURE_WASM",
        "_POSIX_C_SOURCE=200112L",
        "_DEFAULT_SOURCE",
        "_BSD_SOURCE",
        "_DARWIN_C_SOURCE"
      ],
      "cflags_c": ["-std=c11"],
      "xcode_settings": {
        "GCC_C_LANGUAGE_STANDARD": "c11"
      },
      "msvs_settings": {
        "VCCLCompilerTool": {
          "AdditionalOptions!": ["-std:c++20"],
          "AdditionalOptions": ["/std:c11"],
          "DisableSpecificWarnings": ["4018", "4232", "4244", "4267", "4701"]
        }
      },
      "conditions": [
        ["OS=='win'", {
          "defines": [
            "WASM_API_EXTERN=",
            "WASI_API_EXTERN="
          ]
        }]
      ]
    },
    {
      "target_name": "document-engine",
      "dependencies": ["tree-sitter-runtime", "pcre2-runtime"],
      "sources": [
        "src/bindings/addon.cc",
        "src/bindings/document-session.cc",
        "src/bindings/display-view.cc",
        "src/display/display-index.cc",
        "src/display/render-plan.cc",
        "src/revision-projection.cc",
        "src/syntax/injection-engine.cc",
        "src/syntax/js-regex.cc",
        "src/syntax/layered-query.cc",
        "src/syntax/query-engine.cc",
        "src/syntax/query-snapshot-cache.cc",
        "src/syntax/scope-resolver.cc",
        "src/syntax/syntax-backend.cc",
        "src/text-bridge/snapshot-reader.cc",
        "src/testing/snapshot-lease-test-provider.cc"
      ],
      "include_dirs": [
        "src",
        "<!(node script/resolve-superstring-abi.js --print-include)",
        "<(tree_sitter_root)/lib/include",
        "<(tree_sitter_root)/lib/src",
        "<(tree_sitter_root)/lib/src/wasm",
        "<(wasmtime_root)/include",
        "<!(node -p \"require('node-addon-api').include_dir\")"
      ],
      "defines": [
        "NAPI_VERSION=<(napi_build_version)",
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        "NODE_API_SWALLOW_UNTHROWABLE_EXCEPTIONS"
      ],
      "cflags_cc": ["-std=c++20", "-fexceptions"],
      "xcode_settings": {
        "CLANG_CXX_LANGUAGE_STANDARD": "c++20",
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES"
      },
      "msvs_settings": {
        "VCCLCompilerTool": {
          "AdditionalOptions": ["/std:c++20"],
          "ExceptionHandling": 1
        }
      },
      "conditions": [
        ["OS=='win'", {
          "defines": [
            "NOMINMAX",
            "WASM_API_EXTERN=",
            "WASI_API_EXTERN="
          ],
          "libraries": [
            "<(wasmtime_root)/lib/wasmtime.lib",
            "ws2_32.lib",
            "advapi32.lib",
            "userenv.lib",
            "ntdll.lib",
            "shell32.lib",
            "ole32.lib",
            "bcrypt.lib"
          ]
        }],
        ["OS=='linux'", {
          "libraries": [
            "<(wasmtime_root)/lib/libwasmtime.a",
            "-lpthread",
            "-ldl",
            "-lm"
          ]
        }],
        ["OS=='mac'", {
          "libraries": [
            "<(wasmtime_root)/lib/libwasmtime.a"
          ]
        }]
      ]
    }
  ]
}
