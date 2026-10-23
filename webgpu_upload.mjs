export function packDirtyRect(memory, offset, screenW, screenH, dx, dy, dw, dh) {
  if (![offset, screenW, screenH, dx, dy, dw, dh].every(Number.isSafeInteger) ||
      offset < 0 || screenW <= 0 || screenH <= 0 || dx < 0 || dy < 0 ||
      dw <= 0 || dh <= 0 || dx + dw > screenW || dy + dh > screenH) {
    throw new RangeError('Invalid screen buffer or dirty rectangle');
  }

  const buffer = memory.buffer;
  const screenBytes = screenW * screenH * 4;
  const start = offset + (dy * screenW + dx) * 4;
  const rowBytes = dw * 4;
  const packedBytes = rowBytes * dh;
  if (!Number.isSafeInteger(screenBytes) || !Number.isSafeInteger(start) ||
      !Number.isSafeInteger(packedBytes) || offset + screenBytes > buffer.byteLength) {
    throw new RangeError('Screen buffer exceeds WebAssembly memory');
  }
  const packed = new Uint8Array(packedBytes);
  for (let row = 0; row < dh; row++) {
    const source = start + row * screenW * 4;
    if (source + rowBytes > buffer.byteLength) {
      throw new RangeError('Dirty rectangle exceeds WebAssembly memory');
    }
    packed.set(new Uint8Array(buffer, source, rowBytes), row * rowBytes);
  }
  return packed;
}
