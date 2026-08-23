import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./app";
import { closeLoader } from "./h2loader_client";
import "./i18n";
import "./styles.css";

globalThis.addEventListener("pagehide", () => { void closeLoader().catch(() => {}); }, { once: true });

createRoot(document.getElementById("root")).render(<StrictMode><App /></StrictMode>);
