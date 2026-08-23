import { useEffect, useRef } from "react";
import * as ProgressPrimitive from "@radix-ui/react-progress";
import { cn } from "../../lib/utils";

export function Progress({ value = 0, className, ...props }) {
  const normalized = Math.max(0, Math.min(100, Number(value) || 0));
  const indicatorRef = useRef(null);

  // The shipped archive sets a strict CSP (style-src 'self') which blocks style
  // attributes, so drive the indicator through the CSSOM instead.
  useEffect(() => {
    if (indicatorRef.current) {
      indicatorRef.current.style.transform = `translateX(-${100 - normalized}%)`;
    }
  }, [normalized]);

  return (
    <ProgressPrimitive.Root
      className={cn("h-1.5 w-[150px] shrink-0 overflow-hidden rounded-full bg-track", className)}
      value={normalized}
      {...props}>
      <ProgressPrimitive.Indicator
        ref={indicatorRef}
        className="h-full w-full rounded-full bg-info transition-transform duration-200"
      />
    </ProgressPrimitive.Root>
  );
}
