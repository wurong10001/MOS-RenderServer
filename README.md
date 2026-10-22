# RenderServer — 多租户 WASM 渲染服务

根据仓库架构图实现的核心模块：一个 WASM 渲染服务，管理 framebuffer 池、窗口树、合成与脏区，并向应用暴露 C ABI、向宿主暴露 WASM 导入函数。

架构（见仓库根目录架构图）：

  - WASM RenderServer：管理 framebuffer 池、窗口树、合成、脏区；暴露 C ABI / WASM 导入函数
  - WASM App A / B / C：每个只画自己的窗口 buffer
  - JS Host / WebGPU：事件转发、帧同步、最终提交

## 目录结构

  - render_server.h / render_server.c —— 核心实现（零 libc 依赖，自带 arena 分配器与 memcpy/memset）
  - apps/app_a.c, app_b.c, app_c.c —— 三个示例 app，每个只画自己的窗口 buffer
  - rs_demo.c —— 单模块（RS_MONOLITHIC）整合构建时把三个 app 注册进 server，用于 WASM3 在设备上验证
  - webgpu_host.mjs / index.html —— JS / WebGPU 宿主
  - build.sh —— 编译到 wasm32
  - out/ —— 构建产物

## 构建（wasm32）

    bash build.sh

产物：

  - render_server.wasm：仅 RenderServer 本体，导出 memory 与全部 rs_* 函数，从 env 导入 host_log / host_get_time / host_submit
  - app_a.wasm / app_b.wasm / app_c.wasm：独立 app 模块，从 render_server 模块导入 rs_*，并通过 --import-memory 共享 server 的线性内存
  - render_server_demo.wasm：单模块整合版（server + apps），可由 WASM3 直接运行验证

## 在设备上验证（WASM3）

    bash run_test.sh

加载 render_server_demo.wasm，链接宿主函数，运行若干帧，并校验合成结果像素：各窗口底色、z-order 叠放（C 在 B 之上）、背景，以及脏区优化路径。

## JS / WebGPU 宿主

用任意静态服务器在本目录起服务（WebGPU 需要安全上下文，localhost 即可）：

    python3 -m http.server 8080

浏览器打开 http://localhost:8080/ 。宿主会：

  1. 实例化 render_server.wasm（提供 env.host_* 导入）
  2. 依次实例化 app_a/b/c.wasm，用 server 导出的 rs_* 作为 render_server 模块导入，并共享 server 的内存（--import-memory）
  3. 每帧驱动各 app 的 frame()，再调用 server.rs_composite()
  4. server 合成后回调 host_submit(屏幕偏移, w, h, 脏矩形) —— 宿主把脏矩形写入 WebGPU 纹理并提交呈现
  5. 指针事件经 hit-test 路由给顶层窗口所属的 app

## 应用侧 C ABI（app 调用）

rs_create_window / rs_window_get_buffer / rs_window_mark_dirty / rs_window_mark_dirty_full / rs_fill_rect / rs_put_pixel / rs_window_set_rect / rs_window_set_pos / rs_window_set_z / rs_window_show / rs_window_focus / rs_destroy_window / rs_window_x / rs_window_y / rs_window_width / rs_window_height。

关键约束：app 只能通过 rs_window_get_buffer(id) 拿到自己那个窗口的 buffer 地址，因此天然只画自己的窗口 buffer。

## 宿主侧 API（JS Host 调用）

rs_init / rs_composite / rs_frame / rs_dispatch_event / rs_hit_test / rs_get_screen_buffer / rs_get_screen_w / rs_get_screen_h / rs_get_dirty_x/y/w/h / rs_window_*。

## 设计要点

  - 像素格式 RGBA8，字节序 R,G,B,A，与现有 sample.wasm 约定一致。
  - framebuffer 池：在固定 arena 上的 bump + 空闲链表分配器（无 libc、无 malloc）。
  - 脏区：每次标记把局部区域并入一个全局脏矩形；合成时只重绘与脏矩形相交的可见窗口，自底向上不透明合成。
  - 独立模块 app 通过 --import-memory 与 server 共享同一块线性内存，因此窗口 buffer 的偏移量在跨模块间有效。
