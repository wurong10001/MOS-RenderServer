#!/usr/bin/env bash
# RenderServer 验证入口:
#   1) build.sh 编出 wasm32 产物
#   2) Node WebAssembly 运行 test_render_server.mjs 做像素/ABI 断言
#      (BASE: v1 合成行为; V2: 实例/能力/事件队列/fb/配额/元数据)
# 旧版脚本依赖仓库中不存在的 wasm3 C harness, 已替换。
# 设备内嵌验证(mos-android wasm3)可在集成时基于同一 out/render_server_demo.wasm 复用。
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v clang >/dev/null || ! command -v wasm-ld >/dev/null; then
    echo "需要 clang + wasm-ld (LLVM, 建议 >=14): apt install clang lld" >&2
    exit 127
fi

bash build.sh

if command -v node >/dev/null; then
    node test_render_server.mjs out
else
    echo "需要 node >=16 运行验证器 test_render_server.mjs" >&2
    exit 127
fi
