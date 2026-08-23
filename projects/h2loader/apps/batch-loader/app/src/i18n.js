import i18n from "i18next";
import LanguageDetector from "i18next-browser-languagedetector";
import { initReactI18next } from "react-i18next";
import en from "./locales/en.json";
import zhCN from "./locales/zh-CN.json";

export const LANGUAGES = [
  { code: "en", label: "EN" },
  { code: "zh-CN", label: "中文" },
];

export const LANGUAGE_STORAGE_KEY = "h2loader.batch-loader.language";

i18n
  .use(LanguageDetector)
  .use(initReactI18next)
  .init({
    resources: { en: { translation: en }, "zh-CN": { translation: zhCN } },
    supportedLngs: LANGUAGES.map((item) => item.code),
    fallbackLng: { zh: ["zh-CN", "en"], "zh-TW": ["zh-CN", "en"], "zh-HK": ["zh-CN", "en"], default: ["en"] },
    initImmediate: false,
    interpolation: { escapeValue: false },
    detection: {
      order: ["localStorage", "navigator"],
      lookupLocalStorage: LANGUAGE_STORAGE_KEY,
      caches: ["localStorage"],
    },
  });

i18n.on("languageChanged", (language) => {
  document.documentElement.lang = language;
});
document.documentElement.lang = i18n.resolvedLanguage || "en";

export default i18n;
