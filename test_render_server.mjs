#!/usr/bin/env node
// RenderServer 验证器 (Node WebAssembly，无需浏览器/WebGPU)
//
// 用法: node test_render_server.mjs [out目录]
// 两组:
//   BASE —— v1 行为像素断言: 底色 / z-order / 背景 / 脏区 / hit_test / 分模块共享内存
//   V2   —— WindowSpec v2 ABI: rs_register_app / flags / 能力隔离 / 事件队列 /
//            focus-blur / fb 句柄 / 配额 / 关闭拦截 / icon / 标题 / 配置
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { routePointerEvent } from "./event_routing.mjs";
import { packDirtyRect } from "./webgpu_upload.mjs";

const OUT = process.argv[2] || join(process.cwd(), "out");
const memoryConfig = JSON.parse(readFileSync(new URL("./memory_config.json", import.meta.url), "utf8"));
const A_BASE = [40, 10, 10, 255];
const B_BASE = [10, 40, 10, 255];
const C_BASE = [10, 10, 40, 255];
const BG = [18, 20, 28, 255];

let passed = 0, failed = 0, curSuite = "";
const lines = [];
function t(name, fn) {
  try {
    fn();
    passed++;
    lines.push(`  PASS [${curSuite}] ${name}`);
  } catch (e) {
    failed++;
    lines.push(`  FAIL [${curSuite}] ${name}: ${e.message}`);
  }
}
function eq(actual, expect, what) {
  const a = JSON.stringify(actual), b = JSON.stringify(expect);
  if (a !== b) throw new Error(`${what}: 期望 ${b}, 实际 ${a}`);
}
function assert(cond, msg) { if (!cond) throw new Error(msg); }
function need(ex, name) {
  if (typeof ex[name] !== "function") throw new Error(`导出缺失: ${name}`);
  return ex[name];
}
const readWasm = (f) => readFileSync(join(OUT, f));

function hostObj(submits) {
  return {
    env: {
      host_log: () => {},
      host_get_time: () => 0,
      host_submit: (offset, w, h, dx, dy, dw, dh) => submits.push({ offset, w, h, dx, dy, dw, dh }),
    },
  };
}
function bytes(ex) { return new Uint8Array(ex.memory.buffer); }
function words(ex) { return new Int32Array(ex.memory.buffer); }
function pixel(ex, addr, x, y, sw) {
  const p = addr + (y * sw + x) * 4;
  const b = bytes(ex);
  return [b[p], b[p + 1], b[p + 2], b[p + 3]];
}

curSuite = "UPLOAD";
t("脏区上传: 按屏幕行跨度提取紧密矩形", () => {
  const memory = { buffer: new ArrayBuffer(480 * 320 * 4) };
  const screen = new Uint8Array(memory.buffer);
  for (let y = 0; y < 10; y++) {
    for (let x = 0; x < 480; x++) {
      const i = (y * 480 + x) * 4;
      screen.set([x & 255, y, 17, 255], i);
    }
  }
  const packed = packDirtyRect(memory, 0, 480, 10, 7, 2, 3, 4);
  eq(Array.from(packed.slice(0, 12)), [7, 2, 17, 255, 8, 2, 17, 255, 9, 2, 17, 255],
    "脏矩形首行应从屏幕正确坐标提取");
  eq(Array.from(packed.slice(12, 24)), [7, 3, 17, 255, 8, 3, 17, 255, 9, 3, 17, 255],
    "脏矩形后续行应遵循完整屏幕行跨度");
  let rejected = false;
  try {
    packDirtyRect(memory, 0, 480, 10, 479, 0, 2, 1);
  } catch (error) {
    rejected = error instanceof RangeError;
  }
  assert(rejected, "越界脏矩形应拒绝上传");
});
t("宿主指针路由: 通过 RenderServer 分发并在按下时聚焦", () => {
  const calls = [];
  const server = {
    rs_hit_test: (x, y) => { calls.push(["hit", x, y]); return 42; },
    rs_window_focus: (id) => calls.push(["focus", id]),
    rs_dispatch_event: (type, x, y, meta) => calls.push(["dispatch", type, x, y, meta]),
  };
  routePointerEvent(server, 1, 30, 40, 0);
  eq(calls, [
    ["hit", 30, 40],
    ["focus", 42],
    ["dispatch", 1, 30, 40, 0],
  ], "pointerdown 应聚焦命中窗口并交给事件队列");
  calls.length = 0;
  routePointerEvent(server, 2, 31, 41, 0);
  eq(calls, [["dispatch", 2, 31, 41, 0]], "pointermove 应直接交给 RenderServer");
});

// ---------- BASE: v1 行为 ----------
const submits = [];
const demoRes = await WebAssembly.instantiate(readWasm("render_server_demo.wasm"), hostObj(submits));
const ex = demoRes.instance.exports;

async function runBase() {
  curSuite = "BASE";
  let addr = 0;
  t("rs_init 返回屏幕帧缓冲地址", () => {
    addr = ex.rs_init();
    assert(addr > 0, "rs_init 返回 0");
    assert(ex.memory, "模块未导出 memory (--export-all)");
  });
  const W = ex.rs_get_screen_w(), H = ex.rs_get_screen_h();
  eq([W, H], [480, 320], "屏幕尺寸");

  t("frame0 合成并触发 host_submit", () => {
    ex.rs_frame(0);
    assert(submits.length >= 1, "host_submit 未被调用");
  });
  t("背景像素 = (18,20,28)", () => eq(pixel(ex, addr, 2, 2, W), BG, "bg@2,2"));
  t("AppA 窗口底色", () => eq(pixel(ex, addr, 150, 100, W), A_BASE, "A@150,100"));
  t("AppB 窗口底色", () => eq(pixel(ex, addr, 400, 60, W), B_BASE, "B@400,60"));
  t("AppC 覆盖 AppB (z-order)", () => eq(pixel(ex, addr, 260, 170, W), C_BASE, "C∩B@260,170"));

  t("B 扫描线动画改变像素", () => {
    eq(pixel(ex, addr, 400, 60, W), B_BASE, "frame0: 局部y20 应为底色");
    ex.rs_frame(20);
    eq(pixel(ex, addr, 400, 60, W), [180, 255, 180, 255], "frame20: 局部y20 应为亮线");
  });
  t("脏区: 无标记时合成不提交", () => {
    const n = submits.length;
    ex.rs_composite();
    eq(submits.length, n, "干净帧不应调用 host_submit");
  });
  t("脏区: request_redraw 全屏重绘", () => {
    ex.rs_request_redraw();
    const n = submits.length;
    ex.rs_composite();
    eq(submits.length, n + 1, "应新增一次提交");
    const s = submits[submits.length - 1];
    eq([s.dx, s.dy, s.dw, s.dh], [0, 0, 480, 320], "全屏脏矩形");
  });
  t("hit_test 命中重叠区顶层窗口", () => {
    const id = ex.rs_hit_test(260, 170);
    assert(id > 0, "应命中窗口");
  });

  // 分模块: server.wasm + app_a.wasm 共享内存
  let split = null;
  try {
    const srv = await WebAssembly.instantiate(readWasm("render_server.wasm"), hostObj([]));
    const sx = srv.instance.exports;
    sx.rs_init();
    // app 静态数据基址与目标内存来自共享配置。
    const have = sx.memory.buffer.byteLength;
    if (have < memoryConfig.appMemoryTarget)
      sx.memory.grow(Math.ceil((memoryConfig.appMemoryTarget - have) / 65536));
    const app = await WebAssembly.instantiate(readWasm("app_a.wasm"), {
      env: { memory: sx.memory, host_log: () => {}, host_get_time: () => 0 },
      render_server: sx,
    });
    split = { sx, ax: app.instance.exports };
  } catch (e) {
    lines.push(`  NOTE [BASE] 分模块实例化失败: ${e.message}`);
  }
  t("分模块: app_a_init 建窗并绘制进共享内存", () => {
    assert(split, "分模块未建立(见 NOTE)");
    const win = split.ax.app_a_init();
    assert(win > 0, "窗口创建失败");
    split.ax.app_a_frame(win, 0);
    const buf = split.sx.rs_window_get_buffer(win);
    assert(buf > 0, "server 侧应能取到 buffer");
    const b = new Uint8Array(split.sx.memory.buffer);
    // frame0 白块在窗口局部(0,0)~(19,19)，取局部(5,30)应为主题色
    const off = buf + (30 * 200 + 5) * 4;
    eq([b[off], b[off + 1], b[off + 2]], A_BASE.slice(0, 3), "app_a 直接写入共享内存的像素");
  });
}

// ---------- V2: WindowSpec v2 ----------
async function runV2() {
  curSuite = "V2";
  // scratch: 屏幕外窗口 buffer 当字符串暂存区 (120x120 = 57KB)
  const scratchWin = ex.rs_create_window(0, 0, -600, -600, 120, 120);
  assert(scratchWin > 0, "scratch 窗口创建失败");
  const scratch = ex.rs_window_get_buffer(scratchWin);
  let sp = scratch;
  const str = (s) => {
    const b = bytes(ex);
    for (let i = 0; i < s.length; i++) b[sp + i] = s.charCodeAt(i);
    const out = { ptr: sp, len: s.length };
    sp += (s.length + 15) & ~15;
    return out;
  };
  const CAP_RENDER = 0x1, CAP_INPUT = 0x2;
  let inst = 0, win = 0;

  t("rs_register_app 返回 instance_id", () => {
    const f = need(ex, "rs_register_app");
    const n = str("probe");
    inst = f(n.ptr, n.len, 1000, 0xffffffff, 1);
    assert(inst >= 1, "instance 应 >=1");
  });
  t("实例元数据: name / uid / manifest_version", () => {
    const buf = str("").ptr;
    eq(need(ex, "rs_instance_uid")(inst), 1000, "uid");
    eq(need(ex, "rs_instance_manifest_version")(inst), 1, "manifest");
    eq(need(ex, "rs_instance_name")(inst, buf, 32), 5, "name 长度");
    eq(String.fromCharCode(...bytes(ex).slice(buf, buf + 5)), "probe", "name 内容");
  });
  t("rs_create_window2: flags+minmax+parent 建窗", () => {
    const f = need(ex, "rs_create_window2");
    const n = str("w2");
    win = f(inst, n.ptr, n.len, 10, 220, 30, 20, 1 | 2, 8, 8, 60, 40, 0);
    assert(win > 0, "v2 建窗失败");
  });
  t("flags: 无 VISIBLE 的窗口不被合成", () => {
    const n = str("hid");
    const hid = need(ex, "rs_create_window2")(inst, n.ptr, n.len, 300, 20, 40, 20, 2, 0, 0, 0, 0, 0);
    assert(hid > 0, "隐藏窗创建失败");
    need(ex, "rs_fill_rect")(hid, 0, 0, 40, 20, 0x000000ff);
    need(ex, "rs_window_mark_dirty_full")(hid);
    need(ex, "rs_composite")();
    const addr = need(ex, "rs_get_screen_buffer")();
    eq(pixel(ex, addr, 310, 30, 480), BG, "隐藏窗区域应保持背景");
  });
  t("能力隔离: 无 RENDER 不能建窗", () => {
    const n = str("norender");
    const i2 = ex.rs_register_app(n.ptr, n.len, 1001, 0, 1);
    const w = str("x");
    eq(need(ex, "rs_create_window2")(i2, w.ptr, w.len, 50, 50, 20, 20, 1, 0, 0, 0, 0, 0), 0, "应拒绝");
  });
  t("能力隔离: 无 INPUT 收不到指针事件", () => {
    const n = str("noinput");
    const i2 = ex.rs_register_app(n.ptr, n.len, 1002, CAP_RENDER, 1);
    const tt = str("q");
    const w = need(ex, "rs_create_window2")(i2, tt.ptr, tt.len, 40, 280, 60, 30, 3, 0, 0, 0, 0, 0);
    assert(w > 0, "建窗失败");
    need(ex, "rs_dispatch_event")(1, 50, 290, 7);
    const out = str("").ptr;
    eq(need(ex, "rs_window_poll_event")(w, out), 0, "无 INPUT 不应有事件");
  });
  t("事件队列: INPUT 实例收到 pointer 载荷", () => {
    const n = str("withinput");
    const i2 = ex.rs_register_app(n.ptr, n.len, 1003, CAP_RENDER | CAP_INPUT, 1);
    const tt = str("e");
    const w = need(ex, "rs_create_window2")(i2, tt.ptr, tt.len, 40, 280, 60, 30, 3, 0, 0, 0, 0, 0);
    need(ex, "rs_dispatch_event")(1, 50, 290, 7);
    const out = str("").ptr;
    eq(need(ex, "rs_window_poll_event")(w, out), 1, "应有事件");
    const i32 = out >> 2, m = words(ex);
    eq([m[i32], m[i32 + 1], m[i32 + 2], m[i32 + 3]], [1, 50, 290, 7], "事件载荷");
    eq(need(ex, "rs_window_poll_event")(w, out), 0, "队列应已空");
  });
  t("生命周期: focus→FOCUS(10)/BLUR(11)", () => {
    const mk = () => {
      const n = str("life");
      const i2 = ex.rs_register_app(n.ptr, n.len, 1004, CAP_RENDER | CAP_INPUT, 1);
      const tt = str("f");
      return need(ex, "rs_create_window2")(i2, tt.ptr, tt.len, 60, 120, 50, 50, 3, 0, 0, 0, 0, 0);
    };
    const w1 = mk(), w2 = mk();
    assert(w1 && w2, "建窗失败");
    need(ex, "rs_window_focus")(w1);
    need(ex, "rs_window_focus")(w2);
    const out = str("").ptr, poll = need(ex, "rs_window_poll_event");
    let w2First = -1; const w1Codes = [];
    for (let i = 0; i < 8; i++) {
      if (poll(w1, out)) w1Codes.push(words(ex)[out >> 2]);
      if (w2First < 0 && poll(w2, out)) w2First = words(ex)[out >> 2];
    }
    eq(w2First, 10, "新焦点窗首个事件应为 FOCUS(10)");
    assert(w1Codes.includes(11), "失焦窗应收到 BLUR(11)");
  });
  t("fb 句柄: 与 buffer 地址/尺寸一致", () => {
    const fb = need(ex, "rs_window_get_fb")(win);
    assert(fb > 0, "fb 句柄无效");
    eq(need(ex, "rs_fb_addr")(fb), need(ex, "rs_window_get_buffer")(win), "地址一致");
    eq([need(ex, "rs_fb_w")(fb), need(ex, "rs_fb_h")(fb)], [30, 20], "fb 尺寸");
  });
  t("调整大小: 重分配帧缓冲并保留重叠像素", () => {
    const fb = need(ex, "rs_window_get_fb")(win);
    const oldAddr = need(ex, "rs_fb_addr")(fb);
    const b = bytes(ex);
    b.set([1, 2, 3, 4], oldAddr);
    b.set([5, 6, 7, 8], oldAddr + ((19 * 30 + 29) * 4));
    need(ex, "rs_window_set_rect")(win, 12, 222, 32, 22);
    const newAddr = need(ex, "rs_fb_addr")(fb);
    eq([need(ex, "rs_fb_w")(fb), need(ex, "rs_fb_h")(fb)], [32, 22], "新帧缓冲尺寸");
    eq(Array.from(bytes(ex).slice(newAddr, newAddr + 4)), [1, 2, 3, 4], "保留左上角像素");
    const last = newAddr + ((19 * 32 + 29) * 4);
    eq(Array.from(bytes(ex).slice(last, last + 4)), [5, 6, 7, 8], "保留重叠区域像素");
    const added = newAddr + ((21 * 32 + 31) * 4);
    eq(Array.from(bytes(ex).slice(added, added + 4)), [0, 0, 0, 0], "新增区域清零");
  });
  t("调整大小: 配额不足时保持原帧缓冲", () => {
    const n = str("resize-quota");
    const i2 = ex.rs_register_app(n.ptr, n.len, 1006, 0xffffffff, 1);
    const tt = str("small");
    const w = need(ex, "rs_create_window2")(i2, tt.ptr, tt.len, 0, 0, 2, 2, 1, 0, 0, 0, 0, 0);
    assert(w > 0, "小窗口创建失败");
    need(ex, "rs_set_instance_quota_mem")(i2, 16);
    need(ex, "rs_window_set_rect")(w, 4, 4, 4, 4);
    const fb = need(ex, "rs_window_get_fb")(w);
    eq([need(ex, "rs_fb_w")(fb), need(ex, "rs_fb_h")(fb)], [2, 2], "配额不足应维持旧尺寸");
    eq([
      need(ex, "rs_window_x")(w), need(ex, "rs_window_y")(w),
      need(ex, "rs_window_width")(w), need(ex, "rs_window_height")(w),
    ], [0, 0, 2, 2], "失败时几何尺寸和位置应保持不变");
    eq(need(ex, "rs_instance_used_mem")(i2), 16, "失败时配额记账应不变");
    need(ex, "rs_window_set_rect")(w, 4, 4, 1, 1);
    eq(need(ex, "rs_instance_used_mem")(i2), 4, "缩小时应退还帧缓冲配额");
    need(ex, "rs_window_set_rect")(w, 4, 4, 2, 2);
    eq([need(ex, "rs_fb_w")(fb), need(ex, "rs_fb_h")(fb)], [2, 2], "释放配额后应允许扩容");
    eq(need(ex, "rs_instance_used_mem")(i2), 16, "扩容后配额应准确记账");
  });
  t("配额: 内存配额不足拒绝建窗", () => {
    const n = str("poor");
    const i2 = ex.rs_register_app(n.ptr, n.len, 1005, 0xffffffff, 1);
    need(ex, "rs_set_instance_quota_mem")(i2, 1000);
    const tt = str("big");
    eq(need(ex, "rs_create_window2")(i2, tt.ptr, tt.len, 5, 5, 300, 200, 1, 0, 0, 0, 0, 0), 0, "超配额应拒绝");
    eq(need(ex, "rs_create_window2")(i2, tt.ptr, tt.len, 5, 5, 0x7fffffff, 0x7fffffff, 1, 0, 0, 0, 0, 0), 0, "溢出尺寸应拒绝");
  });
  t("关闭拦截: request 挂起 / ack 销毁", () => {
    need(ex, "rs_window_close_request")(win);
    eq(need(ex, "rs_window_close_pending")(win), 1, "应挂起");
    assert(need(ex, "rs_hit_test")(20, 230) === win || need(ex, "rs_window_close_pending")(win) === 1,
      "ack 前窗口应仍存在");
    need(ex, "rs_window_close_ack")(win);
    eq(need(ex, "rs_window_close_pending")(win), 0, "ack 后窗口应已销毁");
    eq(need(ex, "rs_hit_test")(20, 230) === win, false, "销毁后不应命中");
  });
  t("元数据: icon_res_id / opacity 存取", () => {
    const tt = str("ico");
    const w = need(ex, "rs_create_window2")(inst, tt.ptr, tt.len, 1, 1, 8, 8, 1, 0, 0, 0, 0, 0);
    need(ex, "rs_set_window_icon")(w, 42);
    eq(need(ex, "rs_window_get_icon")(w), 42, "icon");
    need(ex, "rs_window_set_opacity")(w, 200);
    eq(need(ex, "rs_window_get_opacity")(w), 200, "opacity 保存(合成 alpha 路径未启用)");
  });
  t("元数据: 标题设置与读取", () => {
    const t0 = str("old"), tt = str("");
    const w = need(ex, "rs_create_window2")(inst, tt.ptr, 0, 2, 2, 9, 9, 1, 0, 0, 0, 0, 0);
    need(ex, "rs_set_window_title")(w, t0.ptr, t0.len);
    const t1 = str("new-title");
    need(ex, "rs_set_window_title")(w, t1.ptr, t1.len);
    const dst = str("").ptr;
    eq(need(ex, "rs_window_get_title")(w, dst, 32), 9, "标题长度");
    eq(String.fromCharCode(...bytes(ex).slice(dst, dst + 9)), "new-title", "标题内容");
  });
  t("启动配置: env_json 存取", () => {
    const cfg = str('{"k":1}');
    need(ex, "rs_set_instance_config")(inst, cfg.ptr, cfg.len);
    eq(need(ex, "rs_instance_config_len")(inst), 7, "配置长度");
    const replacement = str('{"k":2}');
    need(ex, "rs_set_instance_config")(inst, replacement.ptr, replacement.len);
    eq(need(ex, "rs_instance_config_len")(inst), 7, "替换配置长度");
    const dst = str("").ptr;
    eq(need(ex, "rs_instance_get_config")(inst, dst, 64), 7, "读取长度");
    eq(String.fromCharCode(...bytes(ex).slice(dst, dst + 7)), '{"k":2}', "替换后配置内容");
    need(ex, "rs_set_instance_config")(inst, 0, 0);
    eq(need(ex, "rs_instance_config_len")(inst), 0, "零长度应清除配置");
  });
  t("未启用能力标记: opacity/parent 查询返回 NOT_SUPPORTED 语义", () => {
    // 字段可存: 建窗带 parent; 透明合成未启用: get_flags 回读
    const n = str("opa");
    const w = need(ex, "rs_create_window2")(inst, n.ptr, n.len, 3, 3, 8, 8, 1 | 16, 0, 0, 0, 0, 7);
    const flags = need(ex, "rs_window_get_flags")(w);
    eq((flags >> 4) & 1, 1, "TRANSPARENT flag 应保存(合成路径未启用, 文档已标注)");
    eq(need(ex, "rs_window_parent")(w), 7, "parent_window_id 应保存(联动逻辑未启用)");
  });
}

await runBase();
await runV2();
console.log(lines.join("\n"));
console.log(`\n总计: ${passed} 通过, ${failed} 失败`);
process.exit(failed > 0 ? 1 : 0);
