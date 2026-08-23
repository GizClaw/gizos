import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { MoreHorizontal } from "lucide-react";
import { cn } from "../../lib/utils";

export function Menu({ label, children }) {
  const [open, setOpen] = useState(false);
  const triggerRef = useRef(null);
  const menuRef = useRef(null);

  // The archive ships a strict CSP (style-src 'self'), which blocks style
  // attributes. Position the portal through the CSSOM instead of a style prop.
  const place = useCallback(() => {
    const trigger = triggerRef.current;
    const menu = menuRef.current;
    if (!trigger || !menu) return;
    const rect = trigger.getBoundingClientRect();
    const margin = 8;
    menu.style.position = "fixed";
    menu.style.right = `${Math.max(margin, window.innerWidth - rect.right)}px`;
    // Flip above the trigger when the menu would not fit below, then clamp so
    // it always stays inside the viewport (rows near the bottom edge).
    const height = menu.offsetHeight;
    let top = rect.bottom + 4;
    if (top + height + margin > window.innerHeight) {
      top = rect.top - height - 4;
    }
    top = Math.min(Math.max(margin, top), Math.max(margin, window.innerHeight - height - margin));
    menu.style.top = `${top}px`;
  }, []);

  useLayoutEffect(() => { if (open) place(); }, [open, place]);

  useEffect(() => {
    if (!open) return undefined;
    const onPointer = (event) => {
      if (triggerRef.current?.contains(event.target)) return;
      if (menuRef.current?.contains(event.target)) return;
      setOpen(false);
    };
    const onKey = (event) => { if (event.key === "Escape") setOpen(false); };
    // Keep the menu anchored while the page moves. Dismissing here would also
    // fire when something scrolls the trigger into view before a click.
    const onReflow = () => place();
    document.addEventListener("mousedown", onPointer);
    document.addEventListener("keydown", onKey);
    window.addEventListener("scroll", onReflow, true);
    window.addEventListener("resize", onReflow);
    return () => {
      document.removeEventListener("mousedown", onPointer);
      document.removeEventListener("keydown", onKey);
      window.removeEventListener("scroll", onReflow, true);
      window.removeEventListener("resize", onReflow);
    };
  }, [open, place]);

  return (
    <>
      <button
        ref={triggerRef}
        type="button"
        aria-haspopup="menu"
        aria-expanded={open}
        aria-label={label}
        onClick={() => setOpen((value) => !value)}
        className="grid size-8 place-items-center rounded-md border border-line bg-btn text-fg-2 transition hover:border-line-modal hover:text-fg">
        <MoreHorizontal size={16} />
      </button>
      {open && createPortal(
        <div
          ref={menuRef}
          role="menu"
          className={cn(
            "z-50 min-w-[180px] rounded-lg border border-line-modal bg-card p-1",
            "shadow-[0_18px_50px_rgba(0,0,0,.55)]",
          )}>
          {typeof children === "function" ? children(() => setOpen(false)) : children}
        </div>,
        document.body,
      )}
    </>
  );
}

export function MenuItem({ onSelect, disabled = false, danger = false, accent = false, children }) {
  return (
    <button
      type="button"
      role="menuitem"
      disabled={disabled}
      onClick={onSelect}
      className={cn(
        "flex w-full items-center gap-2 rounded-md px-2.5 py-1.5 text-left text-[12px] transition",
        accent ? "text-accent" : danger ? "text-danger" : "text-fg-2",
        "enabled:hover:bg-btn enabled:hover:text-fg disabled:cursor-not-allowed disabled:opacity-40",
      )}>
      {children}
    </button>
  );
}

export function MenuSeparator() {
  return <div className="my-1 h-px bg-line" role="separator" />;
}
