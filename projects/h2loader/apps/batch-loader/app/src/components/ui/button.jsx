import { Slot } from "@radix-ui/react-slot";
import { cva } from "class-variance-authority";
import { cn } from "../../lib/utils";

const variants = cva(
  "inline-flex shrink-0 cursor-pointer items-center justify-center gap-1.5 whitespace-nowrap rounded-md font-bold transition outline-none focus-visible:ring-2 focus-visible:ring-accent/60 disabled:cursor-not-allowed disabled:opacity-40",
  {
    variants: {
      variant: {
        default: "bg-accent text-on-accent hover:brightness-110",
        secondary: "border border-line-strong bg-btn text-fg hover:border-line-modal hover:bg-btn-sm",
        ghost: "text-fg-2 hover:bg-btn hover:text-fg",
        destructive: "bg-danger-bg text-danger hover:brightness-125",
      },
      size: {
        default: "h-9 px-4 text-[12px]",
        sm: "h-7 px-2.5 text-[11px]",
        icon: "size-8",
      },
    },
    defaultVariants: { variant: "default", size: "default" },
  },
);

export function Button({ asChild = false, className, variant, size, ...props }) {
  const Component = asChild ? Slot : "button";
  return <Component className={cn(variants({ variant, size }), className)} {...props} />;
}
