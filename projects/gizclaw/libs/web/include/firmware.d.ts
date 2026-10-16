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
  /** Cancel this client's pending calls; the firmware and other clients keep running. */
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
/** Low-level ABI provided by the firmware itself, with no npm dependency. */
export interface GizClawFirmwareResponse {
  code: number;
  hasError: boolean;
  rpcCode: number;
  rpcMessage: string;
  payload: Uint8Array;
  nativeTools: number[] | null;
}
export interface GizClawFirmwareABI {
  readonly version: number;
  readonly request_capacity: number;
  readonly request_limit_bytes: number;
  readonly timeout_ms: number;
  readonly closed: boolean;
  /** Positive ID, or negative PAL admission error with no callback.
   * Accepted callbacks run once asynchronously and synchronously return the
   * delivery result: 0 after successful consumption, negative on failure.
   * Request/reply bytes are copied. Cancel/timeout cannot undo writes.
   */
  request(method: number, payload: Uint8Array,
    callback: (response: GizClawFirmwareResponse) => number,
    options?: {timeout_ms?: number}): number;
  /** Cancel an accepted call; its callback receives code -10 exactly once. */
  cancel(id: number): number;
}
export interface GizClawFirmwareRuntime {
  /** Set by h2_web_gizclaw_open; null after close. Wait for product readiness. */
  readonly h2GizClaw: GizClawFirmwareABI | null;
}
/** Bind a running WASM Module. No iframe or global runtime lookup is required.
 * Multiple clients may coexist. close() cancels only this client's calls.
 */
export function createFirmwareClient(runtime: GizClawFirmwareRuntime): GizClawFirmwareClient;
