import DefaultTheme from "vitepress/theme";
import { inBrowser, type Theme } from "vitepress";
import "./style.css";
import { installDocImageLightbox } from "./image-lightbox";

export default {
  extends: DefaultTheme,
  enhanceApp() {
    if (inBrowser) {
      installDocImageLightbox();
    }
  },
} satisfies Theme;
