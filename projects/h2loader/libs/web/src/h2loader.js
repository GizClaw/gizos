import createH2LoaderModule from "./h2loader_runtime.js";

const WOULD_BLOCK = -9;
const ERROR_NAMES = new Map([
  [-1, "invalid-argument"],
  [-2, "unavailable"],
  [-3, "unsupported"],
  [-4, "io"],
  [-5, "out-of-memory"],
  [-6, "timeout"],
  [-7, "invalid-state"],
  [-8, "not-found"],
  [-9, "would-block"],
  [-10, "closed"],
  [-13, "no-space"],
  [-18, "busy"],
  [1, "cancelled"],
]);

export class H2LoaderError extends Error {
  constructor(code, operation, detail = "") {
    const kind = ERROR_NAMES.get(code) ?? "unknown";
    super(`${operation} failed (${kind}${detail ? `: ${detail}` : ""})`);
    this.name = "H2LoaderError";
    this.code = code;
    this.kind = kind;
    this.operation = operation;
    this.detail = detail;
  }
}

const delay = (milliseconds) =>
  new Promise((resolve) => globalThis.setTimeout(resolve, milliseconds));
const PHASES = ["idle", "connect", "precheck", "stage", "activate", "rediscover", "final-verify", "complete"];
const immutable = (value) => Object.freeze(value);

export async function createH2Loader(options = {}) {
  if (options === null || typeof options !== "object" || Array.isArray(options) ||
      Object.keys(options).some((key) => key !== "pollIntervalMs")) {
    throw new H2LoaderError(-1, "create", "expected only the optional pollIntervalMs field");
  }
  const pollIntervalMs = options.pollIntervalMs ?? 20;
  if (!Number.isInteger(pollIntervalMs) || pollIntervalMs < 1 || pollIntervalMs > 1000) {
    throw new H2LoaderError(-1, "create", "pollIntervalMs must be an integer from 1 to 1000");
  }
  const wasmUrl = new URL("./h2loader_runtime.wasm", import.meta.url);
  const module = await createH2LoaderModule({
    locateFile(path) {
      return path.endsWith(".wasm") ? wasmUrl.href : path;
    },
    noInitialRun: true,
  });
  module.h2LoaderWebBlobs = new Map();
  const client = module.ccall("h2_h2loader_web_create", "number", [], []);
  if (!client) throw new H2LoaderError(-5, "create");

  let nextBlobHandle = 1;
  let closed = false;
  let closePromise;
  const portIds = new WeakMap();
  const portObjects = new Map();

  const call = (name, returnType, argumentTypes, arguments_) =>
    module.ccall(name, returnType, argumentTypes, arguments_);

  const ensureOpen = () => {
    if (closed) throw new H2LoaderError(-10, "client");
  };

  const wrapPort = (entry) => {
    const existing = portObjects.get(entry.id);
    if (existing) return existing;
    const port = Object.freeze({
      label: entry.label || "Authorized serial device",
      usbVendorId: entry.usbVendorId || 0,
      usbProductId: entry.usbProductId || 0,
    });
    portIds.set(port, entry.id);
    portObjects.set(entry.id, port);
    return port;
  };

  const unwrapPort = (port, operation) => {
    const id = portIds.get(port);
    if (!id) throw new H2LoaderError(-1, operation, "invalid port object");
    return id;
  };

  const retainBlob = (blob) => {
    if (!(blob instanceof Blob) || blob.size <= 0 || blob.size > 0xffffffff) {
      throw new H2LoaderError(-1, "package", "expected a non-empty Blob");
    }
    const handle = nextBlobHandle++;
    module.h2LoaderWebBlobs.set(handle, blob);
    return handle;
  };

  const runJob = async (operation, start, {signal, onProgress} = {}) => {
    ensureOpen();
    if (signal?.aborted) throw new DOMException("Aborted", "AbortError");
    const handle = start();
    if (!handle) throw new H2LoaderError(-18, operation, "no free operation slot");
    const abort = () => {
      call("h2_h2loader_web_job_cancel", "number", ["number", "number"], [
        client,
        handle,
      ]);
    };
    signal?.addEventListener("abort", abort, {once: true});
    try {
      for (;;) {
        if (closed) throw new H2LoaderError(-10, operation);
        const done = call(
          "h2_h2loader_web_job_done",
          "number",
          ["number", "number"],
          [client, handle],
        );
        if (done < 0) throw new H2LoaderError(done, operation);
        if (typeof onProgress === "function") {
          const progressJson = call(
            "h2_h2loader_web_job_progress_json",
            "string",
            ["number", "number"],
            [client, handle],
          );
          if (progressJson) {
            const progress = JSON.parse(progressJson);
            const percent = progress.totalBytes
              ? Math.floor((progress.acknowledgedBytes * 100) / progress.totalBytes)
              : 0;
            onProgress(immutable({
              ...progress,
              phase: PHASES[progress.phase] ?? "unknown",
              percent,
              progress: percent,
            }));
          }
        }
        if (done) break;
        await delay(pollIntervalMs);
      }
      const code = call(
        "h2_h2loader_web_job_result",
        "number",
        ["number", "number"],
        [client, handle],
      );
      const json = call(
        "h2_h2loader_web_job_json",
        "string",
        ["number", "number"],
        [client, handle],
      );
      const result = json
        ? JSON.parse(json)
        : {};
      if (code !== 0) {
        if (signal?.aborted || code === 1) {
          throw new DOMException("Aborted", "AbortError");
        }
        throw new H2LoaderError(code, operation, result.detail ?? "");
      }
      return result;
    } finally {
      signal?.removeEventListener("abort", abort);
      if (!closed) {
        call(
          "h2_h2loader_web_job_release",
          "number",
          ["number", "number"],
          [client, handle],
        );
      }
    }
  };

  const normalizeStatus = (status = {}) => ({
    ...status,
    role: status.role === 1 ? "h2loader" : status.role === 2 ? "app" : "unknown",
  });

  const api = {
    supported() {
      return Boolean(globalThis.isSecureContext && globalThis.navigator?.serial);
    },

    async requestPort() {
      ensureOpen();
      const code = call(
        "h2_h2loader_web_request_port",
        "number",
        ["number"],
        [client],
      );
      if (code !== 0) throw new H2LoaderError(code, "request-port");
      for (;;) {
        const result = call(
          "h2_h2loader_web_authorization_result",
          "number",
          ["number"],
          [client],
        );
        if (result === WOULD_BLOCK) {
          await delay(pollIntervalMs);
          ensureOpen();
          continue;
        }
        if (result === -8) throw new DOMException("Port selection cancelled", "NotFoundError");
        if (result !== 0) throw new H2LoaderError(result, "request-port");
        const id = call(
          "h2_h2loader_web_authorization_port",
          "string",
          ["number"],
          [client],
        );
        const ports = await api.getAuthorizedPorts();
        const selected = ports.find((port) => portIds.get(port) === id);
        return selected ?? wrapPort({id});
      }
    },

    async forgetPort(port) {
      ensureOpen();
      const id = unwrapPort(port, "forget-port");
      const code = call(
        "h2_h2loader_web_forget_port",
        "number",
        ["number", "string"],
        [client, id],
      );
      if (code !== 0) throw new H2LoaderError(code, "forget-port");
      for (;;) {
        const result = call(
          "h2_h2loader_web_forget_result",
          "number",
          ["number"],
          [client],
        );
        if (result === WOULD_BLOCK) {
          await delay(pollIntervalMs);
          ensureOpen();
          continue;
        }
        if (result !== 0) throw new H2LoaderError(result, "forget-port");
        portObjects.delete(id);
        portIds.delete(port);
        return;
      }
    },

    async getAuthorizedPorts() {
      const result = await runJob("get-authorized-ports", () =>
        call("h2_h2loader_web_list_ports", "number", ["number"], [client]),
      );
      return immutable(result.ports.map(wrapPort));
    },

    async inspectPackage(blob, jobOptions = {}) {
      const blobHandle = retainBlob(blob);
      try {
        return immutable(await runJob(
          "inspect-package",
          () =>
            call(
              "h2_h2loader_web_inspect_package",
              "number",
              ["number", "number", "number"],
              [client, blobHandle, blob.size],
            ),
          jobOptions,
        ));
      } finally {
        module.h2LoaderWebBlobs.delete(blobHandle);
      }
    },

    status(port, jobOptions = {}) {
      const id = unwrapPort(port, "status");
      return runJob(
        "status",
        () =>
          call(
            "h2_h2loader_web_status",
            "number",
            ["number", "string"],
            [client, id],
          ),
        jobOptions,
      ).then((result) => immutable(normalizeStatus(result.status)));
    },

    async install(port, blob, jobOptions = {}) {
      const id = unwrapPort(port, "install");
      const blobHandle = retainBlob(blob);
      try {
        const result = await runJob(
          "install",
          () =>
            call(
              "h2_h2loader_web_install",
              "number",
              ["number", "string", "number", "number"],
              [client, id, blobHandle, blob.size],
            ),
          jobOptions,
        );
        return immutable({...normalizeStatus(result.status), detail: result.detail});
      } finally {
        module.h2LoaderWebBlobs.delete(blobHandle);
      }
    },
    async stage(port, blob, jobOptions = {}) {
      const id = unwrapPort(port, "stage");
      const blobHandle = retainBlob(blob);
      try {
        const result = await runJob(
          "stage",
          () =>
            call(
              "h2_h2loader_web_stage",
              "number",
              ["number", "string", "number", "number"],
              [client, id, blobHandle, blob.size],
            ),
          jobOptions,
        );
        return immutable({...normalizeStatus(result.status), detail: result.detail});
      } finally {
        module.h2LoaderWebBlobs.delete(blobHandle);
      }
    },

    rollback(port, jobOptions = {}) {
      const id = unwrapPort(port, "rollback");
      return runJob(
        "rollback",
        () =>
          call(
            "h2_h2loader_web_rollback",
            "number",
            ["number", "string"],
            [client, id],
          ),
        jobOptions,
      ).then(immutable);
    },

    restart(port, jobOptions = {}) {
      const id = unwrapPort(port, "restart");
      return runJob(
        "restart",
        () =>
          call(
            "h2_h2loader_web_restart",
            "number",
            ["number", "string"],
            [client, id],
          ),
        jobOptions,
      ).then(immutable);
    },

    rebootLoader(port, jobOptions = {}) {
      const id = unwrapPort(port, "reboot-loader");
      return runJob(
        "reboot-loader",
        () =>
          call(
            "h2_h2loader_web_reboot_loader",
            "number",
            ["number", "string"],
            [client, id],
          ),
        jobOptions,
      ).then(immutable);
    },
    rebootApp(port, jobOptions = {}) {
      const id = unwrapPort(port, "reboot-app");
      return runJob(
        "reboot-app",
        () =>
          call(
            "h2_h2loader_web_reboot_app",
            "number",
            ["number", "string"],
            [client, id],
          ),
        jobOptions,
      ).then(immutable);
    },

    close() {
      if (closePromise) return closePromise;
      closed = true;
      call("h2_h2loader_web_close_begin", "number", ["number"], [client]);
      closePromise = (async () => {
        for (let turn = 0; turn < 1024; ++turn) {
          const result = call(
            "h2_h2loader_web_close_step",
            "number",
            ["number"],
            [client],
          );
          if (result === WOULD_BLOCK) {
            await delay(1);
            continue;
          }
          module.h2LoaderWebBlobs.clear();
          portObjects.clear();
          if (result !== 0 && result !== -3) {
            throw new H2LoaderError(result, "close");
          }
          return;
        }
        throw new H2LoaderError(-6, "close", "shutdown did not converge");
      })();
      return closePromise;
    },
  };

  return Object.freeze(api);
}
