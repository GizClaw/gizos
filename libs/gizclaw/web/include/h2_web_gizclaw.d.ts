import type {
  ClientHwdID, ClientHwdMap, ClientToolID, ClientToolMap,
  CLIENT_HWD_IDS, CLIENT_TOOL_IDS,
} from './method-map';

type Hwd = ClientHwdID | keyof typeof CLIENT_HWD_IDS;
type HwdNumber<K extends Hwd> = K extends keyof typeof CLIENT_HWD_IDS
  ? (typeof CLIENT_HWD_IDS)[K] : K & ClientHwdID;
type Tool = ClientToolID | keyof typeof CLIENT_TOOL_IDS;
type ToolNumber<K extends Tool> = K extends keyof typeof CLIENT_TOOL_IDS
  ? (typeof CLIENT_TOOL_IDS)[K] : K & ClientToolID;

export interface GizClawCallOptions {
  signal?: AbortSignal;
  /** 1..120000 ms; default selected by the entry. Cancellation cannot undo writes. */
  timeout_ms?: number;
}
export interface GizClawError extends Error {
  /** Original signed PAL execution result, or -1000 for a wire-only rejection. */
  code: number;
  rpcCode?: number;
  rpcMessage?: string;
}
export interface GizClawWebSDK {
  readonly version: 1;
  call(method: number, request: Uint8Array, options?: GizClawCallOptions): Promise<Uint8Array>;
  readonly mhs: {
    read<K extends Hwd>(request: {id: string; hwd: K}, options?: GizClawCallOptions): Promise<{
      payload: ClientHwdMap[HwdNumber<K>]['readResponse'];
      write_capabilities?: {fields: string[]};
    }>;
    write<K extends Hwd>(request: {
      id: string;
      hwd: K;
      payload: ClientHwdMap[HwdNumber<K>]['writeRequest'];
    }, options?: GizClawCallOptions): Promise<{
      payload: ClientHwdMap[HwdNumber<K>]['writeResponse'];
    }>;
  };
  readonly tools: {
    list(options?: GizClawCallOptions): Promise<Array<{id: ClientToolID; name: string}>>;
    invoke<K extends Tool>(tool: K, request: ClientToolMap[ToolNumber<K>]['request'],
      options?: GizClawCallOptions): Promise<ClientToolMap[ToolNumber<K>]['response']>;
  };
}
export interface GizClawLuaAppSummary {
  app_id: string;
  version: string;
  display_name: string;
  description: string;
}
export interface GizClawWebLuaApps {
  readonly version: 1;
  list(): Promise<GizClawLuaAppSummary[]>;
  install(source: Blob | Uint8Array | ArrayBuffer, options?: {sha256?: string}): Promise<GizClawLuaAppSummary>;
  uninstall(appId: string): Promise<void>;
}
declare global {
  var h2WebGizClaw: GizClawWebSDK | undefined;
  var h2WebLuaApps: GizClawWebLuaApps | undefined;
}
