import { defineConfig } from "vitepress";
import { withMermaid } from "vitepress-mermaid-plugin";
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, URL } from "node:url";

const repositoryRoot = fileURLToPath(new URL("../../", import.meta.url));
const guidesRoot = path.join(repositoryRoot, "guides");

function copiedSourcePageExists(url: string, sourceFile: string): boolean {
  const clean = url.replace(/[?#].*$/, "").replace(/\.(?:html|md)$/, "");
  const candidate = clean.startsWith("/")
    ? path.join(guidesRoot, clean)
    : path.resolve(path.dirname(sourceFile), clean);
  return existsSync(`${candidate}.md`) || existsSync(path.join(candidate, "index.md"));
}

const zhDevelopmentItems = [
  { text: "总览", link: "/zh/guide" },
  {
    text: "开发应用",
    items: [
      {
        text: "使用 Runtime 开发固件",
        link: "/zh/developing/app/runtime",
      },
      {
        text: "使用 LVGL 开发固件",
        link: "/zh/developing/app/lvgl",
      },
    ],
  },
  {
    text: "平台抽象层",
    link: "/zh/developing/platform_abstract_layer",
  },
  { text: "Runtime", link: "/zh/developing/runtime" },
  { text: "Drivers", link: "/zh/developing/drivers" },
  {
    text: "Libraries",
    items: [
      { text: "总览", link: "/zh/developing/library" },
      {
        text: "App Test",
        link: "/zh/developing/app_test",
        collapsed: true,
        items: [
          {
            text: "实现",
            link: "/zh/developing/app_test/implementation",
          },
        ],
      },
      { text: "Audio Mixer", link: "/zh/developing/audio_mixer" },
      { text: "BLE iKCP", link: "/zh/developing/bleikcp" },
      { text: "BLE Wi-Fi Config", link: "/zh/developing/ble_wifi_config" },
      { text: "Command", link: "/zh/developing/command" },
      { text: "IO Stream iKCP", link: "/zh/developing/iostreamikcp" },
      { text: "JSON", link: "/zh/developing/json" },
      { text: "Bundle", link: "/zh/developing/bundle" },
      { text: "CoreMQTT", link: "/zh/developing/coremqtt" },
      { text: "DNS", link: "/zh/developing/dns" },
      { text: "FFmpeg", link: "/zh/developing/ffmpeg" },
      { text: "Game Runtime", link: "/zh/developing/game_runtime" },
      { text: "GizClaw", link: "/zh/developing/gizclaw" },
      { text: "H2Peer", link: "/zh/developing/h2peer" },
      { text: "H2SCTP", link: "/zh/developing/h2sctp" },
      { text: "libSRTP", link: "/zh/developing/libsrtp" },
      { text: "LVGL", link: "/zh/developing/lvgl" },
      { text: "MP4 Decoder", link: "/zh/developing/mp4_decoder" },
      { text: "iperf", link: "/zh/developing/iperf" },
      { text: "NTP", link: "/zh/developing/ntp" },
      { text: "PIXA", link: "/zh/developing/pixa" },
      { text: "PortAudio", link: "/zh/developing/portaudio" },
      { text: "QR Code", link: "/zh/developing/qrcode" },
      { text: "SDL3", link: "/zh/developing/sdl3" },
      { text: "SQLite", link: "/zh/developing/sqlite" },
      { text: "TinyH264", link: "/zh/developing/tinyh264" },
      { text: "Utils", link: "/zh/developing/utils" },
    ],
  },
  {
    text: "Components",
    items: [
      { text: "总览", link: "/zh/developing/components" },
      { text: "Linux", link: "/zh/developing/components/linux" },
      { text: "Darwin", link: "/zh/developing/components/darwin" },
      { text: "POSIX", link: "/zh/developing/components/posix" },
      { text: "Windows", link: "/zh/developing/components/windows" },
      { text: "Desktop", link: "/zh/developing/components/desktop" },
      {
        text: "ESP-IDF 6.x",
        link: "/zh/developing/components/esp_idf6_x",
      },
      { text: "BK7258", link: "/zh/developing/components/bk7258" },
      { text: "BK3633", link: "/zh/developing/components/bk3633" },
      { text: "JieLi", link: "/zh/developing/components/jieli" },
    ],
  },
  { text: "目录结构", link: "/zh/developing/repo_layout" },
  { text: "Bazel CI 依赖图", link: "/zh/developing/bazel" },
];

const appItems = [
  { text: "总览", link: "/apps/" },
  { text: "文档规范", link: "/apps/documentation" },
  { text: "Embedded Linux", link: "/apps/embed_linux" },
  {
    text: "H2Loader",
    collapsed: true,
    items: [
      { text: "总览", link: "/apps/h2loader/" },
      {
        text: "项目结构",
        link: "/apps/h2loader/project_structure",
      },
      {
        text: "固件结构分区与类型",
        link: "/apps/h2loader/firmware_types",
      },
      {
        text: "更新、启动与回退",
        link: "/apps/h2loader/update/",
        collapsed: true,
        items: [
          { text: "总览", link: "/apps/h2loader/update/" },
          { text: "App 更新", link: "/apps/h2loader/update/app" },
          { text: "Loader 更新", link: "/apps/h2loader/update/loader" },
        ],
      },
      {
        text: "Boards",
        collapsed: true,
        items: [
          { text: "总览", link: "/apps/h2loader/boards/" },
          {
            text: "AMOLED",
            collapsed: true,
            items: [
              { text: "总览", link: "/apps/h2loader/boards/amoled/" },
              { text: "H2Loader", link: "/apps/h2loader/boards/amoled/h2loader" },
              { text: "Display", link: "/apps/h2loader/boards/amoled/display" },
              { text: "QR Code", link: "/apps/h2loader/boards/amoled/qrcode" },
              { text: "Audio System", link: "/apps/h2loader/boards/amoled/audio_system" },
              { text: "GizClaw Ping Speed", link: "/apps/h2loader/boards/amoled/gizclaw_ping_speed" },
              { text: "Crash Before Confirm", link: "/apps/h2loader/boards/amoled/crash_before_confirm" },
              { text: "iperf", link: "/apps/h2loader/boards/amoled/iperf" },
              { text: "WebRTC Performance", link: "/apps/h2loader/boards/amoled/webrtc_performance" },
            ],
          },
          {
            text: "BK7258 V3 202405",
            collapsed: true,
            items: [
              { text: "总览", link: "/apps/h2loader/boards/bk7258_v3_202405/" },
              { text: "H2Loader", link: "/apps/h2loader/boards/bk7258_v3_202405/h2loader" },
              { text: "Display", link: "/apps/h2loader/boards/bk7258_v3_202405/display" },
              { text: "Audio System", link: "/apps/h2loader/boards/bk7258_v3_202405/audio_system" },
              { text: "Crash Before Confirm", link: "/apps/h2loader/boards/bk7258_v3_202405/crash_before_confirm" },
            ],
          },
          {
            text: "DevKit",
            collapsed: true,
            items: [
              { text: "总览", link: "/apps/h2loader/boards/devkit/" },
              { text: "H2Loader", link: "/apps/h2loader/boards/devkit/h2loader" },
            ],
          },
          {
            text: "SZP",
            collapsed: true,
            items: [
              { text: "总览", link: "/apps/h2loader/boards/szp/" },
              { text: "H2Loader", link: "/apps/h2loader/boards/szp/h2loader" },
              { text: "Display", link: "/apps/h2loader/boards/szp/display" },
              { text: "Audio System", link: "/apps/h2loader/boards/szp/audio_system" },
              { text: "Crash Before Confirm", link: "/apps/h2loader/boards/szp/crash_before_confirm" },
              { text: "Partial Update Smoke", link: "/apps/h2loader/boards/szp/partial_update_smoke" },
            ],
          },
          {
            text: "Waveshare ESP32-S3-A7670E-4G",
            collapsed: true,
            items: [
              { text: "总览", link: "/apps/h2loader/boards/waveshare_esp32s3_a7670e_4g/" },
              { text: "H2Loader", link: "/apps/h2loader/boards/waveshare_esp32s3_a7670e_4g/h2loader" },
              { text: "BLE Broadcaster", link: "/apps/h2loader/boards/waveshare_esp32s3_a7670e_4g/ble_broadcaster" },
              { text: "BLE Observer", link: "/apps/h2loader/boards/waveshare_esp32s3_a7670e_4g/ble_observer" },
              { text: "Modem Smoke", link: "/apps/h2loader/boards/waveshare_esp32s3_a7670e_4g/modem_smoke" },
              { text: "Crash Before Confirm", link: "/apps/h2loader/boards/waveshare_esp32s3_a7670e_4g/crash_before_confirm" },
            ],
          },
          {
            text: "Waveshare ESP32-P4",
            collapsed: true,
            items: [
              { text: "总览", link: "/apps/h2loader/boards/waveshare_esp32p4_wifi6_touch_lcd_4_3/" },
              { text: "H2Loader", link: "/apps/h2loader/boards/waveshare_esp32p4_wifi6_touch_lcd_4_3/h2loader" },
              { text: "Display", link: "/apps/h2loader/boards/waveshare_esp32p4_wifi6_touch_lcd_4_3/display" },
              { text: "Audio System", link: "/apps/h2loader/boards/waveshare_esp32p4_wifi6_touch_lcd_4_3/audio_system" },
              { text: "Crash Before Confirm", link: "/apps/h2loader/boards/waveshare_esp32p4_wifi6_touch_lcd_4_3/crash_before_confirm" },
            ],
          },
        ],
      },
      {
        text: "Apps",
        collapsed: true,
        items: [
          {
            text: "Batch Loader",
            link: "/apps/h2loader/apps/batch_loader/",
          },
          { text: "BLE iKCP Baseline", link: "/apps/h2loader/apps/bleikcp_speed/" },
          { text: "Wi-Fi CSI Smoke", link: "/apps/h2loader/apps/wifi_csi/" },
        ],
      },
    ],
  },
  {
    text: "GizClaw",
    collapsed: true,
    items: [
      { text: "总览", link: "/apps/gizclaw" },
      { text: "Peer Connection 传输拓扑", link: "/apps/gizclaw/transport" },
      { text: "状态与请求", link: "/apps/gizclaw/state" },
      { text: "Audio System", link: "/apps/gizclaw/audio" },
      { text: "OTA", link: "/apps/gizclaw/ota" },
    ],
  },
  {
    text: "Showcase 展架程序",
    collapsed: true,
    items: [
      { text: "总览", link: "/apps/showcase" },
      { text: "App 生命周期", link: "/apps/showcase/lifecycle" },
      { text: "Display 与背景视频", link: "/apps/showcase/display" },
      { text: "对话", link: "/apps/showcase/conversation" },
      { text: "触屏控制台", link: "/apps/showcase/console" },
      { text: "资源与持久化", link: "/apps/showcase/resources" },
      {
        text: "Board Spec",
        link: "/apps/showcase/board/",
        collapsed: true,
        items: [
          { text: "总览", link: "/apps/showcase/board/" },
          { text: "KICKPI K4B", link: "/apps/showcase/board/k4b" },
        ],
      },
    ],
  },
  {
    text: "拍学机",
    collapsed: true,
    items: [{ text: "Prototyping", link: "/apps/paixueji" }],
  },
  {
    text: "PIXA Games",
    collapsed: true,
    items: [
      { text: "总览", link: "/apps/pixa_games" },
      { text: "DinoRun", link: "/apps/pixa_games/dinorun" },
      { text: "DinoDive", link: "/apps/pixa_games/dinodive" },
      { text: "DinoBounce", link: "/apps/pixa_games/dinobounce" },
      { text: "DinoTetris", link: "/apps/pixa_games/dinotetris" },
      { text: "Polygon Battle", link: "/apps/pixa_games/polygon_battle" },
      { text: "Tuxemon", link: "/apps/pixa_games/tuxemon" },
    ],
  },
];

const zhCodingStyleItems = [
  { text: "总览", link: "/zh/coding-styles/" },
  { text: "命名", link: "/zh/coding-styles/naming" },
  { text: "C", link: "/zh/coding-styles/c" },
  { text: "Go", link: "/zh/coding-styles/go" },
  { text: "Markdown", link: "/zh/coding-styles/markdown" },
];

const zhReviewItems = [
  { text: "总览", link: "/zh/reviewing/" },
  { text: "审查项目", link: "/zh/reviewing/review_items" },
  { text: "开发后自我审查", link: "/zh/reviewing/self_review" },
  { text: "PR Agent 审查", link: "/zh/reviewing/pr_agent_review" },
  { text: "Issue 审查", link: "/zh/reviewing/issue_review" },
];

const zhUsageItems = [
  { text: "总览", link: "/zh/using/" },
  {
    text: "Embedded Linux",
    items: [
      { text: "ADB 总览", link: "/zh/using/embed_linux/" },
      { text: "KICKPI K4B", link: "/zh/using/embed_linux/kickpi_k4b" },
    ],
  },
  {
    text: "H2Loader",
    items: [
      { text: "总览与环境", link: "/zh/using/h2loader/" },
      { text: "CLI", link: "/zh/using/h2loader/cli" },
    ],
  },
];

const referenceItems = [
  { text: "总览", link: "/references/" },
  {
    text: "libs/",
    items: [
      { text: "App Test", link: "/references/app_test" },
      { text: "Audio Mixer", link: "/references/audio_mixer" },
      { text: "BLE iKCP", link: "/references/bleikcp" },
      { text: "BLE Wi-Fi Config", link: "/references/ble_wifi_config" },
      { text: "Command", link: "/references/command" },
      { text: "Bundle", link: "/references/bundle" },
      { text: "CoreMQTT", link: "/references/coremqtt" },
      { text: "DNS", link: "/references/dns" },
      { text: "FFmpeg", link: "/references/ffmpeg" },
      { text: "Drivers", link: "/references/drivers" },
      { text: "Game Runtime", link: "/references/game_runtime" },
      { text: "GizClaw", link: "/references/gizclaw" },
      { text: "H2Peer", link: "/references/h2peer" },
      { text: "H2SCTP", link: "/references/h2sctp" },
      { text: "IO Stream iKCP", link: "/references/iostreamikcp" },
      { text: "libSRTP", link: "/references/libsrtp" },
      { text: "LVGL", link: "/references/lvgl" },
      { text: "MP4 Decoder", link: "/references/mp4_decoder" },
      { text: "iperf", link: "/references/iperf" },
      { text: "NTP", link: "/references/ntp" },
      { text: "PAL", link: "/references/pal" },
      { text: "PIXA", link: "/references/pixa" },
      { text: "PortAudio", link: "/references/portaudio" },
      { text: "QR Code", link: "/references/qrcode" },
      { text: "Runtime", link: "/references/runtime" },
      { text: "SDL3", link: "/references/sdl3" },
      { text: "SQLite", link: "/references/sqlite" },
      { text: "TinyH264", link: "/references/tinyh264" },
      { text: "Utils", link: "/references/utils" },
      { text: "WolfSSL", link: "/references/wolfssl" },
    ],
  },
  {
    text: "projects/h2loader/libs/",
    items: [{ text: "H2Loader", link: "/references/h2loader" }],
  },
];

const zhThemeConfig = {
  nav: [
    { text: "开发指引", link: "/zh/guide" },
    { text: "产品文档", link: "/apps/" },
    { text: "审核指引", link: "/zh/reviewing/" },
    { text: "编码规范", link: "/zh/coding-styles/" },
    { text: "使用说明", link: "/zh/using/" },
    { text: "Reference", link: "/references/" },
  ],
  sidebar: {
    "/zh/guide": [{ text: "开发指引", items: zhDevelopmentItems }],
    "/zh/developing/": [{ text: "开发指引", items: zhDevelopmentItems }],
    "/apps/": appItems,
    "/zh/using/": [{ text: "使用说明", items: zhUsageItems }],
    "/zh/reviewing/": [
      { text: "审核指引", items: zhReviewItems },
    ],
    "/zh/coding-styles/": [
      { text: "编码规范", items: zhCodingStyleItems },
    ],
    "/references/": [{ text: "API Reference", items: referenceItems }],
  },
  outline: {
    label: "本页目录",
    level: [2, 4] as [number, number],
  },
  docFooter: {
    prev: "上一页",
    next: "下一页",
  },
  lastUpdated: {
    text: "最后更新",
  },
  langMenuLabel: "切换语言",
  returnToTopLabel: "返回顶部",
  sidebarMenuLabel: "菜单",
  darkModeSwitchLabel: "主题",
  lightModeSwitchTitle: "切换到浅色模式",
  darkModeSwitchTitle: "切换到深色模式",
};

const enThemeConfig = {
  nav: [
    { text: "Home", link: "/" },
    { text: "中文", link: "/zh/" },
    { text: "Reference", link: "/references/" },
  ],
  sidebar: {
    "/en/": [
      {
        text: "English",
        items: [{ text: "Overview", link: "/en/" }],
      },
    ],
  },
  search: {
    provider: "local" as const,
  },
};

export default withMermaid(
  defineConfig({
    title: "GizOS 项目文档",
    description: "GizOS 项目开发指引、App 开发文档、使用说明和 API Reference",
    cleanUrls: true,
    srcDir: guidesRoot,
    ignoreDeadLinks: [copiedSourcePageExists],
    lastUpdated: true,
    srcExclude: ["env/**", "h106/**", "users/**"],
    locales: {
      root: {
        label: "简体中文",
        lang: "zh-CN",
        title: "GizOS 项目文档",
        description: "GizOS 项目开发指引、App 开发文档、使用说明和 API Reference",
        themeConfig: {
          ...zhThemeConfig,
          search: {
            provider: "local" as const,
          },
        },
      },
      en: {
        label: "English",
        lang: "en-US",
        link: "/en/",
        title: "GizOS Project Guide",
        description: "GizOS project documentation",
        themeConfig: enThemeConfig,
      },
    },
    mermaid: {
      theme: "default",
    },
    vite: {
      server: {
        fs: {
          allow: [repositoryRoot],
        },
      },
    },
    themeConfig: {
      ...zhThemeConfig,
      search: {
        provider: "local",
      },
    },
  }),
);
