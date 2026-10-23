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
  - build.sh —— 编译到 wasm32（-fno-builtin-memset/memcpy：避免 -nostdlib 下
    clang 把字节循环变成 env.memset 导入导致宿主实例化失败）
  - test_render_server.mjs —— Node WebAssembly 验证器（BASE+V2 断言）
  - out/ —— 构建产物

## 构建（wasm32）

    bash build.sh        # 需要 clang + wasm-ld + node

产物：

  - render_server.wasm：仅 RenderServer 本体，导出 memory 与全部 rs_* 函数，从 env 导入 host_log / host_get_time / host_submit
  - app_a.wasm / app_b.wasm / app_c.wasm：独立 app 模块，从 render_server 模块导入 rs_*，并通过 --import-memory 共享 server 的线性内存
  - render_server_demo.wasm：单模块整合版（server + apps），可由 WASM3 直接运行验证

## 验证

    bash run_test.sh        # 需要 clang + wasm-ld + node

先 build.sh 编出 wasm32 产物，再由 test_render_server.mjs 在 Node WebAssembly 中运行
render_server_demo.wasm 与分模块 server/app 组合，验证基础绘制、尺寸调整、脏区复制与 v2 ABI：

- BASE（v1 行为）：背景/AppA/AppB 底色、AppC 盖 AppB 的 z-order、扫描线动画、
  脏区优化（无标记不提交）与全屏重绘、hit_test、分模块共享内存绘制
- V2：实例注册与元数据、能力隔离（RENDER/INPUT）、事件队列、FOCUS/BLUR 生命周期、
  fb 句柄、内存配额、关闭拦截、icon/opacity/标题/env_json 存取、未启用字段回读

旧版脚本依赖仓库中不存在的 wasm3 C harness，已替换；设备内嵌验证（mos-android
wasm3）可基于同一 out/render_server_demo.wasm 复用。

## JS / WebGPU 宿主

用任意静态服务器在本目录起服务（WebGPU 需要安全上下文，localhost 即可）：

    python3 -m http.server 8080

浏览器打开 http://localhost:8080/ 。宿主会：

  1. 实例化 render_server.wasm（提供 env.host_* 导入）
  2. 依次实例化 app_a/b/c.wasm，用 server 导出的 rs_* 作为 render_server 模块导入，并共享 server 的内存（--import-memory）
  3. 每帧驱动各 app 的 frame()，再调用 server.rs_composite()
  4. server 合成后回调 host_submit(屏幕偏移, w, h, 脏矩形) —— 宿主把脏矩形写入 WebGPU 纹理并提交呈现
  5. 指针事件经 `rs_dispatch_event` 入目标窗口队列；app 在帧循环中通过 `rs_window_poll_event` 读取

## 应用侧 C ABI（app 调用）

rs_create_window / rs_window_get_buffer / rs_window_mark_dirty / rs_window_mark_dirty_full / rs_fill_rect / rs_put_pixel / rs_window_set_rect / rs_window_set_pos / rs_window_set_z / rs_window_show / rs_window_focus / rs_destroy_window / rs_window_x / rs_window_y / rs_window_width / rs_window_height。

内存约束：示例 apps 与 RenderServer 共享同一 WASM 线性内存。ABI 能力检查只约束经由服务器 API 的调用，不构成应用间内存隔离；任何拥有该共享内存的 app 都可以直接读写其他 app 或服务器的数据。因此此模式只适用于可信应用，不能用作不可信 WASM 的安全沙箱。运行不可信应用时应使用各自独立的线性内存，并通过宿主控制的复制/提交接口通信；宿主还必须控制实例注册与身份映射，不能信任应用自行提供的 instance_id。

## 宿主侧 API（JS Host 调用）

rs_init / rs_composite / rs_frame / rs_dispatch_event / rs_hit_test / rs_get_screen_buffer / rs_get_screen_w / rs_get_screen_h / rs_get_dirty_x/y/w/h / rs_window_*。

## 设计要点

  - 像素格式 RGBA8，字节序 R,G,B,A，与现有 sample.wasm 约定一致。
  - framebuffer 池：在固定 arena 上的 bump + 空闲链表分配器（无 libc、无 malloc）。
  - 脏区：每次标记把局部区域并入一个全局脏矩形；合成时只重绘与脏矩形相交的可见窗口，自底向上不透明合成。
  - 独立模块 app 通过 --import-memory 与 server 共享同一块线性内存，因此窗口 buffer 的偏移量在跨模块间有效。共享内存是性能机制，不是安全边界；能力位不能阻止直接内存访问。

## v2 ABI：多租户身份、能力、事件队列

WindowSpec 思路的 WASM 友好落地（v1 API 全部保留，v1 建窗归入特权 legacy 实例 0）：

- 身份：`rs_register_app(name_ptr,name_len,uid,capabilities,manifest_version)` 返回
  instance_id（PID 语义由 instance_id 承担，uid 做资源归属/配额主体）；
  `rs_instance_uid/caps/manifest_version/name/config` 读写元数据。
  当前示例由 app 调用注册接口，仅用于演示；由于 app 和 server 共享内存且 server ABI 直接导入，
  这不是可信身份认证，不能用于不可信 app。
- 建窗：`rs_create_window2(inst,title,x,y,w,h,flags,min_w,min_h,max_w,max_h,parent)`
  —— 标量 + flags 位域，不用大结构体 ABI。
  flags：VISIBLE(1) FOCUSABLE(2) RESIZABLE(4) MODAL(8) TRANSPARENT(16)。
- 能力强制：无 RENDER 不能建窗/绘制/标脏；无 INPUT 不能获得焦点，也收不到
  POINTER/KEY/FOCUS/BLUR 输入事件。键盘事件发给当前焦点窗口；指针事件按坐标命中，
  命中无 INPUT 的窗口时不向下层窗口穿透。RESIZE/CLOSE_REQ 属于窗口生命周期通知，
  不要求 INPUT。以上检查均在 server 侧执行。
- 配额：`rs_set_instance_quota_mem` 在建窗分配处强制；`rs_instance_used_mem` 查询。
  quota_fps 未实现（宿主限速语义）。
- 回调路由：不用函数指针。事件/生命周期进每窗口 8 格环形队列，
  app 帧循环 `rs_window_poll_event(win,out_ptr)` 取 4×int32(type,x,y,meta)。
  事件：POINTER_DOWN/MOVE/UP、KEY、FOCUS(10)/BLUR(11)、RESIZE(12)、CLOSE_REQ(20)。
  `host.rs_dispatch_event` 即 server 内 hit-test 入队；JS 宿主只需转发原始指针事件。
- 关闭拦截：`rs_window_close_request` 置挂起+投递 CLOSE_REQ，
  app 决定后 `rs_window_close_ack` 才销毁（等价原 on_close 返回值语义）。
- fb 句柄与窗口分离：`rs_window_get_fb` / `rs_fb_addr` / `rs_fb_w` / `rs_fb_h`。
- 分模块内存布局：`memory_config.json` 统一配置 app 静态数据基址和共享内存目标；
  宿主按目标扩展共享内存，避免 app 数据段覆盖 server 的 .data 与 6MB arena。

已定义未启用（字段存取生效、行为待后续）：TRANSPARENT/opacity 的 alpha 合成、
min/max 尺寸 clamp、MODAL、parent 级联（移动/销毁/置顶）、quota_fps、
STORAGE/NETWORK 能力位（server 暂无对应 API，仅参与掩码）。

与 MOS 软件分发的衔接：上传 `uploads/mosapp/*.wasm` 时可在 manifest 中携带
app_id / manifest_version / uid / capabilities / icon_res_id，运行时由宿主读出并注入
`rs_register_app`，即可实现"上传即声明权限"。
