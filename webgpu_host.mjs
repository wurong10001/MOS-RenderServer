// RenderServer —— JS / WebGPU Host
//
// 职责 (对应架构图 JS Host 框):
//   - 实例化 RenderServer WASM (导出 memory + rs_* 等 C ABI)
//   - 实例化各独立 WASM App, 让它们通过 "render_server" 模块导入 rs_*,
//     并共享 RenderServer 的线性内存 (--import-memory)
//   - 帧同步: 每帧驱动各 app 的 frame(), 再调用 server.rs_composite()
//   - host_submit (server 合成后回调) -> 把脏矩形写入 WebGPU 纹理并呈现
//   - 事件转发: 指针事件 -> hit-test -> 路由给顶层窗口所属的 app

const SERVER_URL = './out/render_server.wasm';
const APPS = [
  { name: 'app_a', url: './out/app_a.wasm', init: 'app_a_init', frame: 'app_a_frame', event: 'app_a_event' },
  { name: 'app_b', url: './out/app_b.wasm', init: 'app_b_init', frame: 'app_b_frame', event: 'app_b_event' },
  { name: 'app_c', url: './out/app_c.wasm', init: 'app_c_init', frame: 'app_c_frame', event: 'app_c_event' },
];

function logFromString(mem, ptr, len) {
  const bytes = new Uint8Array(mem.buffer, ptr, len);
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return s;
}

async function loadWasm(url, imports) {
  const res = await fetch(url);
  const bytes = await res.arrayBuffer();
  const result = await WebAssembly.instantiate(bytes, imports);
  return result.instance;
}

const WGSL = `
struct VSOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> VSOut {
  var p = array<vec2f,3>(vec2f(-1.0,-1.0), vec2f(3.0,-1.0), vec2f(-1.0,3.0));
  var o: VSOut;
  o.pos = vec4f(p[i], 0.0, 1.0);
  o.uv = p[i] * 0.5 + vec2f(0.5, 0.5);
  return o;
}
@group(0) @binding(0) var tex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@fragment fn fs(@location(0) uv: vec2f) -> @location(0) vec4f {
  return textureSample(tex, samp, uv);
}
`;

async function main() {
  const status = document.getElementById('status');
  if (!navigator.gpu) { status.textContent = 'WebGPU not available in this browser.'; return; }
  const canvas = document.getElementById('screen');
  const adapter = await navigator.gpu.requestAdapter();
  const device = await adapter.requestDevice();
  const context = canvas.getContext('webgpu');
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: 'opaque' });

  let texture = null, sampler = null, bindGroup = null, pipeline = null, screenW = 0, screenH = 0;

  function ensurePipeline(w, h) {
    if (texture) return;
    texture = device.createTexture({
      size: [w, h, 1],
      format: 'rgba8unorm',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
    sampler = device.createSampler({ magFilter: 'nearest', minFilter: 'nearest' });
    const module = device.createShaderModule({ code: WGSL });
    pipeline = device.createRenderPipeline({
      layout: 'auto',
      vertex: { module, entryPoint: 'vs' },
      fragment: { module, entryPoint: 'fs', targets: [{ format }] },
      primitive: { topology: 'triangle-list' },
    });
    bindGroup = device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: texture.createView() },
        { binding: 1, resource: sampler },
      ],
    });
  }

  let server = null;
  function hostSubmit(offset, w, h, dx, dy, dw, dh) {
    ensurePipeline(w, h);
    const mem = server.exports.memory;
    const src = new Uint8Array(mem.buffer, offset, dw * dh * 4);
    device.queue.writeTexture(
      { texture },
      src,
      { offset: 0, bytesPerRow: dw * 4, rowsPerImage: dh },
      { x: dx, y: dy, z: 0 },
      { width: dw, height: dh, depthOrArrayLayers: 1 }
    );
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({
      colorAttachments: [{
        view: context.getCurrentTexture().createView(),
        clearValue: { r: 0, g: 0, b: 0, a: 1 },
        loadOp: 'clear', storeOp: 'store',
      }],
    });
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, bindGroup);
    pass.draw(3);
    pass.end();
    device.queue.submit([encoder.finish()]);
  }

  const envImports = {
    host_log: (ptr, len) => { console.log('[wasm]', logFromString(server.exports.memory, ptr, len)); },
    host_get_time: () => (performance.now() | 0),
    host_submit: hostSubmit,
  };

  server = await loadWasm(SERVER_URL, { env: envImports });
  screenW = server.exports.rs_get_screen_w();
  screenH = server.exports.rs_get_screen_h();
  canvas.width = screenW;
  canvas.height = screenH;
  server.exports.rs_init();

  const appInstances = [];
  for (const a of APPS) {
    const imports = {
      env: {
        memory: server.exports.memory,
        host_log: envImports.host_log,
        host_get_time: envImports.host_get_time,
      },
      render_server: server.exports,
    };
    const inst = await loadWasm(a.url, imports);
    const win = inst.exports[a.init]();
    appInstances.push({ ...a, instance: inst, win });
  }
  status.textContent = 'running: ' + appInstances.length + ' apps composited by RenderServer';

  function routeEvent(type, x, y) {
    const wid = server.exports.rs_hit_test(x, y);
    if (type === 1) server.exports.rs_window_focus(wid);
    for (const ai of appInstances) {
      if (ai.win === wid) { ai.instance.exports[ai.event](wid, type, x, y, 0); return; }
    }
  }
  function localXY(e) {
    const r = canvas.getBoundingClientRect();
    return [ (e.clientX - r.left) * (screenW / r.width) | 0,
             (e.clientY - r.top) * (screenH / r.height) | 0 ];
  }
  canvas.addEventListener('pointerdown', (e) => { const [x, y] = localXY(e); routeEvent(1, x, y); });
  canvas.addEventListener('pointermove', (e) => { const [x, y] = localXY(e); routeEvent(2, x, y); });
  canvas.addEventListener('pointerup',   (e) => { const [x, y] = localXY(e); routeEvent(3, x, y); });

  let frame = 0;
  function tick() {
    for (const ai of appInstances) ai.instance.exports[ai.frame](ai.win, frame);
    server.exports.rs_composite();
    frame++;
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
}

main().catch((e) => { console.error(e); const s = document.getElementById('status'); if (s) s.textContent = 'error: ' + e; });
