const MIN_SCALE = 0.5;
const MAX_SCALE = 6;
const SCALE_STEP = 0.25;

type LightboxState = {
  scale: number;
  x: number;
  y: number;
  dragging: boolean;
  pointerId: number | null;
  pointerX: number;
  pointerY: number;
  restoreFocus: HTMLElement | null;
  restoreOverflow: string;
};

function makeButton(label: string, title: string): HTMLButtonElement {
  const button = document.createElement("button");
  button.type = "button";
  button.className = "doc-image-lightbox__button";
  button.textContent = label;
  button.title = title;
  button.setAttribute("aria-label", title);
  return button;
}

export function installDocImageLightbox(): void {
  if (document.documentElement.dataset.docImageLightbox === "installed") {
    return;
  }
  document.documentElement.dataset.docImageLightbox = "installed";

  const overlay = document.createElement("div");
  overlay.className = "doc-image-lightbox";
  overlay.hidden = true;
  overlay.setAttribute("role", "dialog");
  overlay.setAttribute("aria-modal", "true");
  overlay.setAttribute("aria-label", "图片放大查看");

  const toolbar = document.createElement("div");
  toolbar.className = "doc-image-lightbox__toolbar";

  const title = document.createElement("div");
  title.className = "doc-image-lightbox__title";

  const controls = document.createElement("div");
  controls.className = "doc-image-lightbox__controls";
  const zoomOut = makeButton("−", "缩小");
  const zoomReset = makeButton("100%", "恢复原始缩放");
  const zoomIn = makeButton("+", "放大");
  const closeButton = makeButton("×", "关闭");
  controls.append(zoomOut, zoomReset, zoomIn, closeButton);
  toolbar.append(title, controls);

  const viewport = document.createElement("div");
  viewport.className = "doc-image-lightbox__viewport";
  const image = document.createElement("img");
  image.className = "doc-image-lightbox__image";
  image.draggable = false;
  viewport.append(image);
  overlay.append(toolbar, viewport);
  document.body.append(overlay);

  const state: LightboxState = {
    scale: 1,
    x: 0,
    y: 0,
    dragging: false,
    pointerId: null,
    pointerX: 0,
    pointerY: 0,
    restoreFocus: null,
    restoreOverflow: "",
  };

  function renderTransform(): void {
    image.style.transform = `translate3d(${state.x}px, ${state.y}px, 0) scale(${state.scale})`;
    zoomReset.textContent = `${Math.round(state.scale * 100)}%`;
    viewport.classList.toggle("is-zoomed", state.scale > 1);
  }

  function setScale(nextScale: number): void {
    state.scale = Math.min(MAX_SCALE, Math.max(MIN_SCALE, nextScale));
    if (state.scale <= 1) {
      state.x = 0;
      state.y = 0;
    }
    renderTransform();
  }

  function resetTransform(): void {
    state.scale = 1;
    state.x = 0;
    state.y = 0;
    renderTransform();
  }

  function openLightbox(source: HTMLImageElement): void {
    state.restoreFocus =
      document.activeElement instanceof HTMLElement ? document.activeElement : null;
    state.restoreOverflow = document.body.style.overflow;
    image.src = source.currentSrc || source.src;
    image.alt = source.alt;
    title.textContent = source.alt || "文档原型";
    resetTransform();
    overlay.hidden = false;
    document.body.style.overflow = "hidden";
    closeButton.focus();
  }

  function closeLightbox(): void {
    if (overlay.hidden) {
      return;
    }
    overlay.hidden = true;
    image.removeAttribute("src");
    document.body.style.overflow = state.restoreOverflow;
    state.restoreFocus?.focus();
    state.restoreFocus = null;
  }

  document.addEventListener("click", (event) => {
    if (!(event.target instanceof HTMLImageElement)) {
      return;
    }
    const source = event.target;
    if (
      !source.closest(".vp-doc") ||
      source.closest("a[data-no-lightbox]") ||
      source.classList.contains("no-lightbox")
    ) {
      return;
    }
    event.preventDefault();
    openLightbox(source);
  });

  overlay.addEventListener("click", (event) => {
    if (event.target === overlay || event.target === viewport) {
      closeLightbox();
    }
  });
  closeButton.addEventListener("click", closeLightbox);
  zoomOut.addEventListener("click", () => setScale(state.scale - SCALE_STEP));
  zoomIn.addEventListener("click", () => setScale(state.scale + SCALE_STEP));
  zoomReset.addEventListener("click", resetTransform);

  viewport.addEventListener(
    "wheel",
    (event) => {
      event.preventDefault();
      setScale(state.scale + (event.deltaY < 0 ? SCALE_STEP : -SCALE_STEP));
    },
    { passive: false },
  );
  image.addEventListener("dblclick", () => setScale(state.scale > 1 ? 1 : 2));

  viewport.addEventListener("pointerdown", (event) => {
    if (state.scale <= 1 || event.target !== image) {
      return;
    }
    state.dragging = true;
    state.pointerId = event.pointerId;
    state.pointerX = event.clientX;
    state.pointerY = event.clientY;
    viewport.setPointerCapture(event.pointerId);
    viewport.classList.add("is-dragging");
  });
  viewport.addEventListener("pointermove", (event) => {
    if (!state.dragging || event.pointerId !== state.pointerId) {
      return;
    }
    state.x += event.clientX - state.pointerX;
    state.y += event.clientY - state.pointerY;
    state.pointerX = event.clientX;
    state.pointerY = event.clientY;
    renderTransform();
  });

  function stopDragging(event: PointerEvent): void {
    if (!state.dragging || event.pointerId !== state.pointerId) {
      return;
    }
    state.dragging = false;
    state.pointerId = null;
    viewport.classList.remove("is-dragging");
  }
  viewport.addEventListener("pointerup", stopDragging);
  viewport.addEventListener("pointercancel", stopDragging);

  document.addEventListener("keydown", (event) => {
    if (overlay.hidden) {
      return;
    }
    if (event.key === "Escape") {
      closeLightbox();
    } else if (event.key === "+" || event.key === "=") {
      setScale(state.scale + SCALE_STEP);
    } else if (event.key === "-") {
      setScale(state.scale - SCALE_STEP);
    } else if (event.key === "0") {
      resetTransform();
    }
  });
}
