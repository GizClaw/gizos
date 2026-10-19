import { createFirmwareClient } from '@gizclaw/firmware';
import type { GizClawFirmwareClient, GizClawFirmwareRuntime } from '@gizclaw/firmware';
declare const api: GizClawFirmwareClient;
async function consume() {
  const value = await api.mhs.read({id: 'led.fixture', hwd: 'led'});
  const enabled: boolean | undefined = value.payload.enabled;
  const applied = await api.mhs.write({id: 'led.fixture', hwd: 'led', payload: {
    enabled: false, brightness_percent: 0,
  }});
  const brightness: number | undefined = applied.payload.applied?.brightness_percent;
  const info = await api.tools.invoke('info.get', {});
  const model: string | undefined = info.model;
  await api.tools.invoke('lua.app.run', {app_id: 'demo', params: {mode: 'natural'}});
  await api.tools.invoke('device.find', {});
  // @ts-expect-error boolean presence cannot accept a string
  await api.mhs.write({id: 'led.fixture', hwd: 'led', payload: {enabled: 'false'}});
  // @ts-expect-error the canonical Wi-Fi HWD has no write request
  await api.mhs.write({id: 'wifi.fixture', hwd: 'wifi', payload: {}});
  // @ts-expect-error tool names come from the canonical registry
  await api.tools.invoke('invented.tool', {});
  return [enabled, brightness, model];
}
void consume;
declare const runtime: GizClawFirmwareRuntime;
function bind() {
  const client = createFirmwareClient(runtime);
  void client.tools.list();
  client.close();
  const id = runtime.h2GizClaw?.request(136, new Uint8Array(), response => {
    const code: number = response.code;
    return code;
  });
  if (id && id > 0) runtime.h2GizClaw?.cancel(id);
  // @ts-expect-error An iframe is not a WASM runtime.
  createFirmwareClient(document.createElement('iframe'));
  // @ts-expect-error Delivery must be acknowledged synchronously, not via Promise.
  runtime.h2GizClaw?.request(136, new Uint8Array(), async () => 0);
}
void bind;
