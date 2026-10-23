export function routePointerEvent(server, type, x, y, meta = 0) {
  if (type === 1) {
    const windowId = server.rs_hit_test(x, y);
    if (windowId) server.rs_window_focus(windowId);
  }
  server.rs_dispatch_event(type, x, y, meta);
}
