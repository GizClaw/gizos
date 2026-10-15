import type { GizClawWebSDK } from './h2_web_gizclaw';
declare const api: GizClawWebSDK;
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
