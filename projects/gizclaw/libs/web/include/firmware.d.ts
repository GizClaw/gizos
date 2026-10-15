import type {
  ClientHwdID, ClientHwdMap, ClientToolID, ClientToolMap,
  CLIENT_HWD_IDS, CLIENT_TOOL_IDS,
} from './method-map.js';

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
export interface GizClawFirmwareClient {
  readonly version: 1;
  /** Disconnect this controller; the firmware keeps running. */
  close(): void;
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
export interface GizClawConnectOptions {
  /** Exact iframe origin; defaults to the origin of iframe.src. Opaque origins are unsupported. */
  origin?: string;
  /** Wait for the firmware's registered endpoint; does not start the firmware. Default 30000. */
  timeout_ms?: number;
}
/** Connect only the controlling host to an already embedded firmware page.
 * Different frames are independent. Reconnecting the same frame replaces its
 * previous controller. Call close() before removing/navigating the iframe.
 */
export function connectFirmware(
  iframe: HTMLIFrameElement, options?: GizClawConnectOptions,
): Promise<GizClawFirmwareClient>;
