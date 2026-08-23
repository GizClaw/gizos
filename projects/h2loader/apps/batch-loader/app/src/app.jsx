import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Trans, useTranslation } from "react-i18next";
import {
  ArrowUp, Box, CircleAlert, Hexagon, Info, Package, Pencil, Play, Plus, RefreshCw,
  RotateCcw, ScanLine, Search, Square, Trash2, Upload, Zap,
} from "lucide-react";
import { BatchLoaderController } from "./batch_loader_controller";
import { Button } from "./components/ui/button";
import {
  Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle, DialogTrigger,
} from "./components/ui/dialog";
import { DeviceDetailsDialog } from "./components/device_details";
import { Menu, MenuItem, MenuSeparator } from "./components/ui/menu";
import { Progress } from "./components/ui/progress";
import { getLoader } from "./h2loader_client";
import { LANGUAGES } from "./i18n";
import { canRestart, canStage, canSwitchToApp, canSwitchToLoader } from "./device_actions";
import { mergeAuthorizedPorts } from "./device_merge";
import { loadDeviceMetadata, saveDeviceMetadata } from "./batch_loader_storage";

const MAX_CONCURRENT = 4;
const DEVELOPER_DOCS_URL = "https://github.com/GizClaw/gizos/blob/main/guides/apps/h2loader/apps/batch_loader/index.md";

// Operations that reboot the device; their acknowledged result is stale until rescan.
const REBOOTING_OPS = ["rollback", "restart", "rebootApp"];
// A device is busy while its own operation is queued or running; other devices
// stay free, so single-device work never blocks unrelated rows.
const deviceBusy = (device) => ["queued", "busy"].includes(device.state);
function hex4(value) {
  return Number.isInteger(value) && value > 0 ? value.toString(16).toUpperCase().padStart(4, "0") : "----";
}

function deviceNumber(device) {
  const match = /(\d+)\s*$/.exec(typeof device.portId === "string" ? device.portId : "");
  return match ? `#${match[1]}` : "";
}

function usbId(device) {
  return `${hex4(device.usbVendorId)}:${hex4(device.usbProductId)}`;
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(0)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

const BADGE_CLASSES = {
  ready: "bg-neutral-bg text-fg-2",
  stale: "bg-warn-bg text-warn",
  offline: "bg-neutral-bg text-fg-2",
  queued: "bg-info-bg text-info",
  busy: "bg-info-bg text-info",
  success: "bg-accent-bg text-accent",
  cancelled: "bg-warn-bg text-warn",
  error: "bg-danger-bg text-danger",
};

const BUSY_KEYS = { status: "scanning", stage: "sending", install: "installing", rollback: "switchingToLoader", restart: "restarting", rebootApp: "switchingToApp" };

function StatusBadge({ state, operation }) {
  const { t } = useTranslation();
  const known = BADGE_CLASSES[state] ? state : "ready";
  const key = known === "busy" ? (BUSY_KEYS[operation] || "busy") : known;
  return (
    <span className={`inline-flex items-center gap-1.5 rounded-full px-2.5 py-1 text-[11px] font-bold ${BADGE_CLASSES[known]}`}>
      <span className="size-1.5 rounded-full bg-current" aria-hidden="true" />{t(`status.${key}`)}
    </span>
  );
}

function detailText(t, device) {
  if (device.error) return device.error;
  if (device.state === "busy" && device.phase && device.phase !== "idle") return t("detail.phase", { phase: device.phase });
  if (device.state === "queued") return t("detail.queued");
  if (device.state === "success") return device.lastResult === "stage" ? t("detail.staged") : device.lastResult === "install" ? t("detail.installed") : t("detail.verified");
  if (device.state === "stale" && device.port) return t("detail.restored");
  if (device.state === "offline") return t("detail.offline");
  if (device.state === "cancelled") return t("detail.cancelled");
  return t("detail.notScanned");
}

export function App() {
  const { t, i18n } = useTranslation();
  const [devices, setDevices] = useState(() => loadDeviceMetadata());
  const [packageInfo, setPackageInfo] = useState(null);
  const [packageFile, setPackageFile] = useState(null);
  const [pendingPackage, setPendingPackage] = useState(null);
  const [packageRole, setPackageRole] = useState("app");
  const [packageOpen, setPackageOpen] = useState(false);
  const [packageError, setPackageError] = useState("");
  const [dragActive, setDragActive] = useState(false);
  const [notice, setNotice] = useState("");
  const [sdkReady, setSdkReady] = useState(false);
  const [lifecycleEnded, setLifecycleEnded] = useState(false);
  const [detailsPortId, setDetailsPortId] = useState(null);
  const controller = useRef(null);
  const loaderRef = useRef(null);
  const devicesRef = useRef(devices);
  devicesRef.current = devices;

  if (!controller.current) {
    controller.current = new BatchLoaderController({
      maxConcurrent: MAX_CONCURRENT,
      onUpdate: (portId, patch) => setDevices((items) => items.map((item) =>
        item.portId === portId ? { ...item, ...patch } : item)),
    });
  }

  useEffect(() => saveDeviceMetadata(devices), [devices]);
  useEffect(() => () => controller.current.cancel(), []);
  useEffect(() => {
    const end = () => { setLifecycleEnded(true); controller.current.cancel(); };
    globalThis.addEventListener("pagehide", end);
    return () => globalThis.removeEventListener("pagehide", end);
  }, []);

  const roleLabel = useCallback((role) => (role === "app" || role === "h2loader" ? t(`role.${role}`) : (role || "—")), [t]);

  const refreshPorts = useCallback(async () => {
    try {
      const loader = await getLoader();
      loaderRef.current = loader;
      const supported = loader.supported();
      setSdkReady(supported);
      if (!supported) {
        setNotice(t("notice.unsupported"));
        return [];
      }
      const ports = await loader.getAuthorizedPorts();
      const merged = mergeAuthorizedPorts(ports, devicesRef.current, (index) => t("devices.defaultName", { index }));
      setDevices(merged);
      setNotice(ports.length ? "" : t("notice.noDevice"));
      return merged;
    } catch (error) {
      setNotice(error.message);
      return [];
    }
  }, [t]);

  useEffect(() => { refreshPorts(); }, [refreshPorts]);

  const runOperation = async (operation, targets) => {
    setNotice("");
    const targetIds = new Set(targets.map((item) => item.portId));
    setDevices((items) => items.map((item) =>
      targetIds.has(item.portId) ? { ...item, operation, error: undefined } : item));
    try {
      const loader = await getLoader();
      // Scans only read status, so they are not held to the four-device cap
      // that protects concurrent writes.
      const concurrency = operation === "status" ? targets.length : undefined;
      await controller.current.run(targets, async (device, options) => {
        let result;
        if (operation === "install" || operation === "stage") result = await loader[operation](device.port, packageFile, options);
        else result = await loader[operation](device.port, options);
        // The device's own install state would collide with the row's UI state,
        // so keep it under a separate key for the details dialog.
        const { state: installState, ...rest } = result || {};
        return {
          ...rest,
          ...(installState === undefined ? {} : { installState }),
          lastResult: operation,
          stale: REBOOTING_OPS.includes(operation),
        };
      }, { concurrency });
    } finally {
      setDevices((items) => items.map((item) =>
        targetIds.has(item.portId) && !deviceBusy(item) ? { ...item, operation: undefined } : item));
    }
  };

  const addDevice = async () => {
    setNotice("");
    try {
      const loader = loaderRef.current;
      if (!loader) {
        setNotice(t("notice.sdkStarting"));
        return;
      }
      const port = await loader.requestPort();
      const merged = await refreshPorts();
      const added = merged.find((item) => item.port === port);
      if (added) await runOperation("status", [added]);
    } catch (error) {
      if (error.name !== "NotFoundError") setNotice(error.message);
    }
  };

  const chooseRole = (role) => {
    if (role === packageRole) return;
    setPackageRole(role);
    setPackageFile(null);
    setPackageInfo(null);
    setPendingPackage(null);
    setPackageError("");
  };

  const inspectPackage = async (file) => {
    if (!file) return;
    setPackageError("");
    try {
      const loader = await getLoader();
      const info = await loader.inspectPackage(file);
      if (info.role && info.role !== packageRole) {
        throw new Error(t("dialog.roleMismatch", { expected: packageRole, received: info.role }));
      }
      setPendingPackage({ file, info: { ...info, filename: file.name, size: file.size, role: info.role || packageRole } });
      setNotice("");
    } catch (error) {
      setPendingPackage(null);
      setPackageError(t("dialog.rejected", { message: error.message }));
    }
  };

  const setPackageDialogOpen = (open) => {
    setPackageOpen(open);
    if (!open) {
      setPendingPackage(null);
      setPackageError("");
      setDragActive(false);
    }
  };

  const selectPackage = async (event) => {
    const file = event.target.files?.[0];
    event.target.value = "";
    await inspectPackage(file);
  };

  const dropPackage = async (event) => {
    event.preventDefault();
    setDragActive(false);
    await inspectPackage(event.dataTransfer.files?.[0]);
  };

  const confirmPackage = () => {
    if (!pendingPackage) return;
    setPackageFile(pendingPackage.file);
    setPackageInfo(pendingPackage.info);
    setPendingPackage(null);
    setPackageOpen(false);
  };

  const selected = useMemo(() => devices.filter((item) => item.selected && item.port), [devices]);
  const scannedSelected = useMemo(() => selected.filter((item) => item.role === "app" || item.role === "h2loader"), [selected]);
  const eligible = useMemo(() => ({
    stage: selected.filter(canStage),
    install: selected.filter(canStage),
    rollback: selected.filter(canSwitchToLoader),
    rebootApp: selected.filter(canSwitchToApp),
    restart: selected.filter(canRestart),
  }), [selected]);

  const runBatch = async (operation) => {
    if (!selected.length) { setNotice(t("notice.selectOne")); return; }
    if (operation === "status") {
      await runOperation(operation, selected);
      return;
    }
    // All other operations depend on each device's live role/capabilities.
    const targets = eligible[operation] || [];
    if (!targets.length) {
      setNotice(scannedSelected.length ? t("notice.noEligible") : t("notice.needScan"));
      return;
    }
    if ((operation === "stage" || operation === "install") && !packageFile) { setPackageOpen(true); return; }
    await runOperation(operation, targets);
  };

  const rescanDevice = (device) => runOperation("status", [device]);

  const dropDeviceRow = (portId) => setDevices((items) => items.filter((item) => item.portId !== portId));

  const forgetDevice = async (device) => {
    if (device.port) {
      try {
        const loader = await getLoader();
        await loader.forgetPort(device.port);
      } catch (error) {
        // A port the current SDK client no longer recognizes (e.g. after a
        // client restart) cannot have its authorization revoked here, but the
        // row must still clear. Only surface transport-level failures.
        if (error.code !== -1 && error.code !== -8 && error.name !== "InvalidStateError") {
          setNotice(error.message);
          return;
        }
      }
    }
    dropDeviceRow(device.portId);
  };

  // Cancel never depends on the selection: busy rows cannot be re-selected, so
  // the header cancels everything that is currently running or queued.
  const cancelAll = () => {
    controller.current.cancel();
    setDevices((items) => items.map((item) => deviceBusy(item)
      ? { ...item, state: "cancelled", operation: undefined, error: t("detail.cancelled") } : item));
  };

  const cancelDevice = (device) => {
    controller.current.cancelDevice(device.portId);
    setDevices((items) => items.map((item) => item.portId === device.portId && deviceBusy(item)
      ? { ...item, state: "cancelled", operation: undefined, error: t("detail.cancelled") } : item));
  };

  const toggleAll = (checked) => setDevices((items) => items.map((item) => ({
    ...item, selected: Boolean(checked && item.port),
  })));
  const toggleDevice = (portId, checked) => setDevices((items) => items.map((item) =>
    item.portId === portId ? { ...item, selected: checked } : item));

  const online = useMemo(() => devices.filter((item) => item.port).length, [devices]);
  const anyBusy = useMemo(() => devices.some(deviceBusy), [devices]);
  // A batch can only start when none of its targets is already busy.
  const batchBlocked = useMemo(() => selected.some(deviceBusy), [selected]);
  const canAuthorize = sdkReady && !lifecycleEnded;
  const packageRoleLabel = roleLabel(packageRole);
  const currentLanguage = LANGUAGES.find((item) => i18n.resolvedLanguage?.startsWith(item.code.split("-")[0]))?.code || "en";

  const headerButton = "h-9 px-3.5 text-[13px]";
  const checkboxClass = "size-4 cursor-pointer appearance-none rounded border border-check bg-card checked:border-accent checked:bg-accent checked:bg-[url('data:image/svg+xml;utf8,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 16 16%22><path fill=%22none%22 stroke=%22%23101709%22 stroke-width=%222.2%22 stroke-linecap=%22round%22 stroke-linejoin=%22round%22 d=%22M3.5 8.5l3 3 6-6%22/></svg>')] bg-center bg-no-repeat disabled:cursor-not-allowed disabled:opacity-40";

  return (
    <main className="min-h-screen bg-page font-sans text-fg">
      <header className="sticky top-0 z-20 border-b border-nav-line bg-nav/95 backdrop-blur">
        <div className="mx-auto flex h-[64px] max-w-[1440px] items-center gap-8 px-10">
          <a href="./" className="flex items-center gap-2.5 text-[15px] font-bold tracking-tight">
            <span className="grid size-8 place-items-center rounded-full border border-accent/40 bg-accent/12 text-accent"><Hexagon size={16} /></span>
            <span>{t("nav.brand")}</span>
            <span className="rounded-sm bg-accent/15 px-1.5 py-0.5 text-[11px] font-bold leading-none text-accent">{t("nav.alpha")}</span>
          </a>
          <nav aria-label={t("nav.marketplace")} className="flex h-full items-center gap-7 text-[13px] text-fg-2">
            <span aria-disabled="true">{t("nav.discover")}</span>
            <span aria-disabled="true">{t("nav.categories")}</span>
            <span aria-disabled="true">{t("nav.hardware")}</span>
            <span aria-current="page" className="relative flex h-full items-center font-bold text-accent after:absolute after:inset-x-0 after:bottom-0 after:h-0.5 after:rounded-t after:bg-accent">{t("nav.tools")}</span>
          </nav>
          <label className="ml-auto flex h-9 w-[290px] items-center gap-2 rounded-md border border-line bg-field px-3 text-[12px] text-fg-3">
            <Search size={14} />
            <input className="w-full bg-transparent outline-none placeholder:text-fg-3" placeholder={t("nav.searchPlaceholder")} disabled aria-label={t("nav.searchLabel")} />
            <kbd className="rounded bg-kbd px-1.5 py-0.5 text-[11px] text-fg-2">⌘K</kbd>
          </label>
          <div role="group" aria-label={t("nav.language")} className="flex h-9 items-center rounded-md border border-line bg-field p-0.5 text-[12px] font-bold">
            {LANGUAGES.map((item) => (
              <button key={item.code} type="button" lang={item.code} aria-pressed={currentLanguage === item.code} onClick={() => i18n.changeLanguage(item.code)}
                className={`h-full rounded px-2.5 transition ${currentLanguage === item.code ? "bg-btn text-fg" : "text-fg-3 hover:text-fg-2"}`}>
                {item.label}
              </button>
            ))}
          </div>
          <Button asChild variant="secondary" className={headerButton}><a href={DEVELOPER_DOCS_URL} target="_blank" rel="noreferrer">{t("nav.developerDocs")}</a></Button>
          <Button className={headerButton} disabled={!canAuthorize} onClick={addDevice}>{t("nav.connectDevice")}</Button>
        </div>
      </header>

      <section className="mx-auto max-w-[1440px] px-10 pb-16 pt-6">
        <nav aria-label="Breadcrumb" className="text-[12px] text-fg-3">{t("breadcrumb.marketplace")} <span className="mx-1">/</span> {t("breadcrumb.tools")} <span className="mx-1">/</span> <span className="text-fg-2">{t("breadcrumb.batchLoader")}</span></nav>
        <div className="mt-3 flex items-start justify-between gap-6">
          <div className="flex items-center gap-4">
            <span className="grid size-12 place-items-center rounded-xl border border-accent-line bg-accent-bg-3 text-accent"><Box size={22} /></span>
            <div>
              <h1 className="text-[28px] font-bold leading-tight tracking-tight">{t("page.title")}</h1>
              <p className="mt-1 text-[14px] text-fg-2">{t("page.subtitle")}</p>
            </div>
          </div>

          <Dialog open={packageOpen} onOpenChange={setPackageDialogOpen}>
            <DialogTrigger asChild>
              <button data-testid="current-package" className="flex min-w-[300px] items-center gap-4 rounded-xl border border-line-raised bg-card-raised px-4 py-3 text-left transition hover:border-line-modal">
                <span className="min-w-0 flex-1">
                  <small className="block text-[11px] font-bold uppercase tracking-[.04em] text-fg-3">{t("package.current")}</small>
                  <strong className="mt-1 block truncate text-[13px] font-bold">{packageInfo?.filename || t("package.none")}</strong>
                  <span className="mt-0.5 block truncate text-[11px] text-fg-3">
                    {packageInfo
                      ? [roleLabel(packageInfo.role), packageInfo.board, formatBytes(packageInfo.bytes || packageInfo.size), t("package.verified")].filter(Boolean).join(" · ")
                      : t("package.noneHint")}
                  </span>
                </span>
                <span className="grid size-8 shrink-0 place-items-center rounded-md bg-accent-bg-2 text-accent"><Pencil size={14} /></span>
              </button>
            </DialogTrigger>
            <DialogContent closeLabel={t("dialog.close")}>
              <DialogHeader>
                <DialogTitle>{t("dialog.title")}</DialogTitle>
                <DialogDescription>{t("dialog.description")}</DialogDescription>
              </DialogHeader>
              <div className="mt-5">
                <div className="text-[11px] font-bold uppercase tracking-[.04em] text-fg-3">{t("dialog.roleLabel")}</div>
                <div role="group" aria-label={t("dialog.roleLabel")} className="mt-2 grid grid-cols-2 gap-3">
                  {[["app", t("dialog.roleApp")], ["h2loader", t("dialog.roleLoader")]].map(([role, label]) => {
                    const active = packageRole === role;
                    return (
                      <button key={role} type="button" aria-pressed={active} onClick={() => chooseRole(role)}
                        className={`flex h-11 items-center gap-2.5 rounded-lg border px-4 text-[13px] font-bold transition ${active ? "border-accent-line-2 bg-accent-bg-2 text-fg" : "border-line-raised bg-well-2 text-fg-2 hover:border-line-modal"}`}>
                        <span className={`size-3 rounded-full border-[3px] ${active ? "border-accent bg-on-accent" : "border-radio-off"}`} aria-hidden="true" />
                        {label}
                      </button>
                    );
                  })}
                </div>
              </div>
              <label
                className={`mt-4 flex min-h-[136px] cursor-pointer flex-col items-center justify-center gap-2 rounded-lg border border-dashed p-5 text-center transition ${dragActive ? "border-accent bg-accent-bg-2" : "border-line-dashed-2 bg-well hover:border-fg-3"}`}
                onDragOver={(event) => { event.preventDefault(); setDragActive(true); }}
                onDragLeave={() => setDragActive(false)}
                onDrop={dropPackage}>
                <span className="grid size-9 place-items-center rounded-full bg-circle text-fg-2"><ArrowUp size={16} /></span>
                <strong className="text-[13px] font-bold">{packageRole === "app" ? t("dialog.dropApp") : t("dialog.dropLoader")}</strong>
                <span className="text-[12px] text-fg-2">{t("dialog.browse")} · <code className="font-mono">*.update.tar.zlib</code></span>
                <input data-testid="firmware-file" className="sr-only" type="file" accept=".zlib,.tar.zlib,application/octet-stream" onChange={selectPackage} />
              </label>
              {packageError && (
                <div role="alert" className="mt-3 flex items-start gap-2 rounded-lg border border-danger/40 bg-danger-bg px-3 py-2.5 text-[12px] leading-snug text-danger">
                  <CircleAlert size={15} className="mt-0.5 shrink-0" />{packageError}
                </div>
              )}
              <div className="mt-4 text-[11px] font-bold uppercase tracking-[.04em] text-fg-3">{t("dialog.selected")}</div>
              {pendingPackage ? (
                <div data-testid="package-inspection" className="mt-2 flex items-center gap-3 rounded-lg border border-accent-line-3 bg-accent-well px-4 py-3">
                  <span className="grid size-9 shrink-0 place-items-center rounded-md bg-accent-bg-2 text-accent"><Package size={16} /></span>
                  <span className="min-w-0 flex-1">
                    <strong className="block truncate text-[13px] font-bold">{pendingPackage.info.filename}</strong>
                    <span className="mt-0.5 block truncate text-[11px] text-fg-3">
                      {[roleLabel(pendingPackage.info.role), pendingPackage.info.board || t("dialog.anyBoard"), pendingPackage.info.target || t("dialog.anyTarget"), pendingPackage.info.version || t("dialog.unknownVersion"), formatBytes(pendingPackage.info.bytes || pendingPackage.info.size)].join(" · ")}
                    </span>
                  </span>
                  <span className="inline-flex shrink-0 items-center gap-1.5 rounded-full bg-accent-bg px-2.5 py-1 text-[11px] font-bold text-accent"><span className="size-1.5 rounded-full bg-accent-dot" aria-hidden="true" />{t("dialog.verifiedBadge")}</span>
                </div>
              ) : (
                <div className="mt-2 rounded-lg border border-line bg-well-2 px-4 py-3 text-[12px] text-fg-3">
                  {packageFile && packageInfo?.role === packageRole
                    ? <Trans i18nKey="dialog.keeping" values={{ filename: packageInfo.filename }} components={{ 1: <strong className="text-fg-2" /> }} />
                    : t("dialog.noneVerified")}
                </div>
              )}
              <p className="mt-3 rounded-md bg-well-2 px-3 py-2 text-[11px] text-fg-3">{t("dialog.nextVersion")}</p>
              <DialogFooter>
                <Button variant="secondary" onClick={() => setPackageDialogOpen(false)}>{t("dialog.cancel")}</Button>
                <Button disabled={!pendingPackage} onClick={confirmPackage}>{t("dialog.use", { role: packageRoleLabel })}</Button>
              </DialogFooter>
            </DialogContent>
          </Dialog>
        </div>

        <section className="mt-6 overflow-hidden rounded-2xl border border-line bg-card shadow-[0_24px_70px_rgba(0,0,0,.35)]">
          <div className="flex flex-wrap items-center justify-between gap-4 border-b border-line px-6 py-4">
            <div>
              <h2 className="flex items-center gap-3 text-[17px] font-bold">
                {t("devices.title")}
                <span className="inline-flex items-center gap-1.5 rounded-full bg-accent-bg px-2.5 py-1 text-[11px] font-bold text-accent"><span className="size-1.5 rounded-full bg-accent-dot" aria-hidden="true" />{t("devices.online", { count: online })}</span>
              </h2>
              <p className="mt-1 text-[12px] text-fg-2">{t("devices.subtitle")}</p>
            </div>
            <div className="flex items-center gap-2">
              <Button variant="secondary" className={headerButton} disabled={batchBlocked || !selected.length} onClick={() => runBatch("status")}><ScanLine size={14} /> {t("devices.scan")}</Button>
              <Button variant="secondary" className={headerButton} disabled={!canAuthorize} onClick={addDevice}><Plus size={14} /> {t("devices.add")}</Button>
              <Button variant="secondary" className={headerButton} disabled={batchBlocked || !eligible.rollback.length} onClick={() => runBatch("rollback")}><RotateCcw size={14} /> {t("devices.switchToLoader")}</Button>
              <Button variant="secondary" className={headerButton} disabled={batchBlocked || !eligible.rebootApp.length} onClick={() => runBatch("rebootApp")}><Play size={14} /> {t("devices.switchToApp")}</Button>
              <Button variant="secondary" className={headerButton} disabled={batchBlocked || !eligible.restart.length} onClick={() => runBatch("restart")}><RefreshCw size={14} /> {t("devices.restart")}</Button>
              <Button variant="secondary" className={headerButton} disabled={!anyBusy} onClick={cancelAll}><Square size={12} /> {t("devices.cancel")}</Button>
              <Button variant="secondary" className={headerButton} disabled={batchBlocked || !eligible.stage.length} onClick={() => runBatch("stage")}><Upload size={14} /> {t("devices.send")}</Button>
              <Button className={headerButton} disabled={batchBlocked || !eligible.install.length} onClick={() => runBatch("install")}><Zap size={14} /> {t("devices.sendSwitch")}</Button>
            </div>
          </div>

          {notice && (
            <div className="mx-6 mt-4 flex items-center gap-2 rounded-lg border border-warn/30 bg-warn-bg px-3 py-2.5 text-[12px] text-warn">
              <CircleAlert size={15} />{notice}
            </div>
          )}

          <div className="overflow-x-auto">
            <table className="w-full min-w-[1040px] table-fixed border-collapse text-[12px]">
              <thead>
                <tr className="text-left text-[11px] font-bold uppercase tracking-[.04em] text-fg-3">
                  <th className="w-12 px-6 py-3"><input aria-label={t("devices.selectAll")} type="checkbox" className={checkboxClass} checked={selected.length > 0 && selected.length === online} disabled={anyBusy || !online} onChange={(event) => toggleAll(event.target.checked)} /></th>
                  <th className="w-[240px] px-4 py-3">{t("table.device")}</th>
                  <th className="w-[110px] px-4 py-3">{t("table.board")}</th>
                  <th className="w-[190px] px-4 py-3">{t("table.image")}</th>
                  <th className="w-[130px] px-4 py-3">{t("table.status")}</th>
                  <th className="px-4 py-3">{t("table.progress")}</th>
                  <th className="w-[96px] px-6 py-3 text-right">{t("table.action")}</th>
                </tr>
              </thead>
              <tbody>
                {devices.map((device) => {
                  const percent = device.progress || device.percent || 0;
                  const showProgress = deviceBusy(device) && (device.operation === "stage" || device.operation === "install");
                  return (
                    <tr key={device.portId} data-testid="device-row" className="border-t border-row transition hover:bg-well/60">
                      <td className="px-6 py-3.5"><input aria-label={t("devices.select", { name: device.name })} type="checkbox" className={checkboxClass} checked={device.selected === true} disabled={!device.port || deviceBusy(device)} onChange={(event) => toggleDevice(device.portId, event.target.checked)} /></td>
                      <td className="px-4 py-3.5">
                        <strong className="block font-bold text-fg">{device.name}</strong>
                        <small className="block font-mono text-[11px] text-fg-3">
                          USB {usbId(device)}{device.port ? ` · ${deviceNumber(device)}` : ` · ${t("table.permissionNeeded")}`}
                        </small>
                      </td>
                      <td className="truncate px-4 py-3.5 text-fg">{device.board || "—"}</td>
                      <td className={`truncate px-4 py-3.5 ${device.role ? "text-fg" : "text-fg-3"}`}>{device.role ? `${roleLabel(device.role)} · ${device.activeName ? `${device.activeName} ` : ""}${device.version || t("table.unknownVersion")}` : "—"}</td>
                      <td className="whitespace-nowrap px-4 py-3.5"><StatusBadge state={device.state} operation={device.operation} /></td>
                      <td className="px-4 py-3.5">
                        {showProgress ? (
                          <div className="flex items-center gap-3">
                            <Progress value={percent} aria-label={t("devices.progressLabel", { name: device.name })} />
                            <span className="text-[11px] font-bold text-info">{percent}%</span>
                            {device.totalBytes > 0 && <span className="text-[11px] text-fg-3">{formatBytes(device.acknowledgedBytes || 0)} / {formatBytes(device.totalBytes)}</span>}
                          </div>
                        ) : (
                          <span className={`block max-w-[280px] text-[11px] ${device.error ? "text-danger" : "text-fg-3"}`}>{detailText(t, device)}</span>
                        )}
                      </td>
                      <td className="px-6 py-3.5 text-right">
                        <span className="inline-flex items-center justify-end">
                          <Menu label={t("devices.actionsFor", { name: device.name })}>
                            {(close) => (
                              <>
                                <MenuItem onSelect={() => { close(); setDetailsPortId(device.portId); }}><Info size={14} /> {t("devices.viewStatus")}</MenuItem>
                                <MenuSeparator />
                                <MenuItem disabled={deviceBusy(device) || !device.port} onSelect={() => { close(); rescanDevice(device); }}><ScanLine size={14} /> {t("devices.scan")}</MenuItem>
                                <MenuItem disabled={deviceBusy(device) || !canSwitchToLoader(device)} onSelect={() => { close(); runOperation("rollback", [device]); }}><RotateCcw size={14} /> {t("devices.switchToLoader")}</MenuItem>
                                <MenuItem disabled={deviceBusy(device) || !canSwitchToApp(device)} onSelect={() => { close(); runOperation("rebootApp", [device]); }}><Play size={14} /> {t("devices.switchToApp")}</MenuItem>
                                <MenuItem disabled={deviceBusy(device) || !canRestart(device)} onSelect={() => { close(); runOperation("restart", [device]); }}><RefreshCw size={14} /> {t("devices.restart")}</MenuItem>
                                <MenuItem disabled={deviceBusy(device) || !canStage(device)} onSelect={() => { close(); if (!packageFile) { setPackageOpen(true); } else { runOperation("stage", [device]); } }}><Upload size={14} /> {t("devices.send")}</MenuItem>
                                <MenuItem accent disabled={deviceBusy(device) || !canStage(device)} onSelect={() => { close(); if (!packageFile) { setPackageOpen(true); } else { runOperation("install", [device]); } }}><Zap size={14} /> {t("devices.sendSwitch")}</MenuItem>
                                <MenuSeparator />
                                <MenuItem disabled={!deviceBusy(device)} onSelect={() => { close(); cancelDevice(device); }}><Square size={12} /> {t("devices.cancel")}</MenuItem>
                                <MenuItem danger disabled={deviceBusy(device)} onSelect={() => { close(); forgetDevice(device); }}><Trash2 size={14} /> {t("devices.forget")}</MenuItem>
                              </>
                            )}
                          </Menu>
                        </span>
                      </td>
                    </tr>
                  );
                })}
                {!devices.length && (
                  <tr className="border-t border-row">
                    <td colSpan="7" className="px-6 py-10 text-center text-[12px] text-fg-3">{t("devices.empty")}</td>
                  </tr>
                )}
              </tbody>
            </table>
          </div>

          <div className="mx-6 my-5 flex items-center justify-between gap-4 rounded-lg border border-dashed border-line-dashed bg-well px-4 py-3">
            <div className="flex items-center gap-3">
              <span className="grid size-8 place-items-center rounded-full bg-circle text-fg-2"><Plus size={14} /></span>
              <div>
                <strong className="block text-[12px] font-bold">{t("devices.addAnother")}</strong>
                <span className="block text-[11px] text-fg-3">{t("devices.addAnotherHint")}</span>
              </div>
            </div>
            <Button variant="secondary" size="sm" disabled={!canAuthorize} onClick={addDevice}>{t("devices.authorizePort")}</Button>
          </div>

          <footer className="flex items-center justify-between gap-4 border-t border-line px-6 py-3 text-[11px] text-fg-3">
            <span className="flex items-center gap-2 text-fg-2"><span className="size-1.5 rounded-full bg-accent-dot" aria-hidden="true" />{t("devices.footerLocal")}</span>
            <span>{t("devices.footerPorts", { count: MAX_CONCURRENT })}</span>
          </footer>
        </section>
      </section>

      <DeviceDetailsDialog
        device={devices.find((item) => item.portId === detailsPortId) || null}
        open={detailsPortId !== null}
        onOpenChange={(next) => { if (!next) setDetailsPortId(null); }}
      />
    </main>
  );
}
