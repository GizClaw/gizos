import { useTranslation } from "react-i18next";
import { CircleAlert, CircleCheck, CircleDashed, Info } from "lucide-react";
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle } from "./ui/dialog";
import { appAvailability, capabilityList, statusRows } from "../device_status_view";

const AVAILABILITY_STYLE = {
  installed: "border-accent-line-3 bg-accent-well text-accent",
  running: "border-accent-line-3 bg-accent-well text-accent",
  staged: "border-info/30 bg-info-bg text-info",
  none: "border-warn/30 bg-warn-bg text-warn",
  unknown: "border-line bg-well-2 text-fg-3",
};

export function DeviceDetailsDialog({ device, open, onOpenChange }) {
  const { t } = useTranslation();
  if (!device) return null;
  const scanned = device.role === "app" || device.role === "h2loader";
  const availability = appAvailability(device);
  const rows = statusRows(device);
  const caps = capabilityList(device);
  const dash = t("details.unknownValue");

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent closeLabel={t("details.close")}>
        <DialogHeader>
          <DialogTitle>{t("details.title")}</DialogTitle>
          <DialogDescription>{device.name} · USB {device.usbVendorId ? device.usbVendorId.toString(16).toUpperCase().padStart(4, "0") : "----"}:{device.usbProductId ? device.usbProductId.toString(16).toUpperCase().padStart(4, "0") : "----"}{device.port && /\d+$/.test(device.portId || "") ? ` · #${/(\d+)$/.exec(device.portId)[1]}` : ""}</DialogDescription>
        </DialogHeader>

        {!scanned && (
          <div className="mt-4 flex items-start gap-2 rounded-lg border border-warn/30 bg-warn-bg px-3 py-2.5 text-[12px] text-warn">
            <Info size={15} className="mt-0.5 shrink-0" />{t("details.notScannedHint")}
          </div>
        )}

        <div className="mt-4 text-[11px] font-bold uppercase tracking-[.04em] text-fg-3">{t("details.availability")}</div>
        <div className={`mt-2 rounded-lg border px-4 py-3 text-[13px] font-bold ${AVAILABILITY_STYLE[availability]}`}>
          {t(`details.availabilityText.${availability}`)}
        </div>

        <div className="mt-4 text-[11px] font-bold uppercase tracking-[.04em] text-fg-3">{t("details.firmware")}</div>
        <dl className="mt-2 grid grid-cols-2 gap-x-4 gap-y-2 rounded-lg border border-line bg-well-2 px-4 py-3">
          {rows.map(({ key, value }) => (
            <div key={key} className="min-w-0">
              <dt className="text-[11px] text-fg-3">{t(`details.field.${key}`)}</dt>
              <dd className={`truncate text-[12px] ${value ? "text-fg" : "text-fg-3"}`}>{value || dash}</dd>
            </div>
          ))}
        </dl>

        <div className="mt-4 text-[11px] font-bold uppercase tracking-[.04em] text-fg-3">{t("details.capabilities")}</div>
        <ul className="mt-2 grid grid-cols-2 gap-x-4 gap-y-1.5 rounded-lg border border-line bg-well-2 px-4 py-3">
          {caps.map(({ key, enabled }) => (
            <li key={key} className={`flex items-center gap-2 text-[12px] ${enabled ? "text-fg" : "text-fg-3"}`}>
              {enabled ? <CircleCheck size={14} className="shrink-0 text-accent" /> : <CircleDashed size={14} className="shrink-0" />}
              <span className="truncate">{t(`details.capability.${key}`)}</span>
            </li>
          ))}
        </ul>

        {device.error && (
          <div role="alert" className="mt-4 flex items-start gap-2 rounded-lg border border-danger/40 bg-danger-bg px-3 py-2.5 text-[12px] text-danger">
            <CircleAlert size={15} className="mt-0.5 shrink-0" />{device.error}
          </div>
        )}
      </DialogContent>
    </Dialog>
  );
}
