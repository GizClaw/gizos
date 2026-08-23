import * as DialogPrimitive from "@radix-ui/react-dialog";
import { X } from "lucide-react";
import { cn } from "../../lib/utils";

export const Dialog = DialogPrimitive.Root;
export const DialogTrigger = DialogPrimitive.Trigger;
export const DialogClose = DialogPrimitive.Close;

export function DialogContent({ children, className, closeLabel = "Close", ...props }) {
  return (
    <DialogPrimitive.Portal>
      <DialogPrimitive.Overlay className="fixed inset-0 z-40 bg-overlay/80 backdrop-blur-[2px]" />
      <DialogPrimitive.Content
        className={cn(
          "fixed left-1/2 top-1/2 z-50 w-[min(560px,calc(100vw-40px))] -translate-x-1/2 -translate-y-1/2 rounded-2xl border border-line-modal bg-card p-6 text-fg shadow-[0_30px_90px_rgba(0,0,0,.7)] outline-none",
          className,
        )}
        {...props}>
        {children}
        <DialogPrimitive.Close
          className="absolute right-4 top-4 grid size-8 place-items-center rounded-md bg-close text-fg-2 transition hover:text-fg"
          aria-label={closeLabel}>
          <X size={15} />
        </DialogPrimitive.Close>
      </DialogPrimitive.Content>
    </DialogPrimitive.Portal>
  );
}

export function DialogHeader({ children }) {
  return <div className="pr-10">{children}</div>;
}

export function DialogFooter({ children }) {
  return <div className="-mx-6 -mb-6 mt-5 flex items-center justify-end gap-2 rounded-b-2xl border-t border-line px-6 py-4">{children}</div>;
}

export function DialogTitle({ children }) {
  return <DialogPrimitive.Title className="text-[19px] font-bold tracking-tight">{children}</DialogPrimitive.Title>;
}

export function DialogDescription({ children }) {
  return <DialogPrimitive.Description className="mt-1 text-[12px] text-fg-2">{children}</DialogPrimitive.Description>;
}
