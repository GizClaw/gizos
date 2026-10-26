const MAX_BYTES = 256 * 1024 * 1024;

export function firmwareUrl(value) {
  let url;
  try { url = new URL(value); } catch { throw new Error('请输入有效的 HTTPS 固件包 URL。'); }
  if (url.protocol !== 'https:' || url.username || url.password || url.href.includes('#')) throw new Error('请输入有效的 HTTPS 固件包 URL。');
  return url;
}

export async function downloadFirmwarePackage(value, { signal, onProgress } = {}) {
  const url = firmwareUrl(value);
  const response = await fetch(url, { signal, credentials: 'omit', referrerPolicy: 'no-referrer' });
  if (!response.ok || response.url && new URL(response.url).protocol !== 'https:') throw new Error('固件包下载失败，请检查 URL。');
  const declared = response.headers.get('content-length');
  const total = declared === null ? null : Number(declared);
  if (total !== null && (!Number.isSafeInteger(total) || total <= 0 || total > MAX_BYTES)) throw new Error('固件包超过 256 MiB 或大小无效。');
  const reader = response.body?.getReader();
  if (!reader) throw new Error('固件包下载响应为空。');
  const parts = [];
  let received = 0;
  try {
    while (true) {
      const { done, value: chunk } = await reader.read();
      if (done) break;
      received += chunk.byteLength;
      if (received > MAX_BYTES || total !== null && received > total) {
        await reader.cancel();
        throw new Error('固件包超过 256 MiB 或下载大小不匹配。');
      }
      parts.push(chunk);
      onProgress?.({ receivedBytes: received, totalBytes: total, percent: total === null ? null : received * 100 / total });
    }
  } finally { reader.releaseLock(); }
  if (!received || total !== null && received !== total) throw new Error('固件包下载大小不匹配。');
  let name = 'firmware.tar.zlib';
  try {
    const basename = decodeURIComponent(new URL(response.url || url).pathname.split('/').at(-1));
    if (basename.endsWith('.zlib') && !/[\x00-\x1f\x7f/\\]/.test(basename)) name = basename;
  } catch { /* Opaque download routes use the generic package name. */ }
  return new File(parts, name, { type: 'application/octet-stream' });
}
