#!/usr/bin/env bash
set -euo pipefail
# 在设备上用 WASM3 验证 RenderServer 单模块整合版 (无需浏览器)。
# 用法: bash render_server/run_test.sh

ROOT=app/src/main/cpp/third_party/wasm3

bash render_server/build.sh

mkdir -p render_server/obj
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_api_libc.c -o render_server/obj/m3_api_libc.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_bind.c -o render_server/obj/m3_bind.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_code.c -o render_server/obj/m3_code.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_compile.c -o render_server/obj/m3_compile.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_core.c -o render_server/obj/m3_core.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_deterministic.c -o render_server/obj/m3_deterministic.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_env.c -o render_server/obj/m3_env.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_exec.c -o render_server/obj/m3_exec.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_function.c -o render_server/obj/m3_function.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_info.c -o render_server/obj/m3_info.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_module.c -o render_server/obj/m3_module.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_parse.c -o render_server/obj/m3_parse.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_snapshot.c -o render_server/obj/m3_snapshot.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_validate.c -o render_server/obj/m3_validate.o
clang -std=gnu99 -O2 -Iapp/src/main/cpp/third_party/wasm3 -c app/src/main/cpp/third_party/wasm3/m3_xxh64.c -o render_server/obj/m3_xxh64.o
clang++ -std=gnu++17 -O2 -Iapp/src/main/cpp/third_party/wasm3 tools/test_render_server_wasm.c render_server/obj/*.o -o tools/test_render_server_wasm
./tools/test_render_server_wasm render_server/out/render_server_demo.wasm
