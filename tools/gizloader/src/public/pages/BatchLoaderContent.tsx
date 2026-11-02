import { Button } from '@/components/ui/button'
import { usePublicLocale } from '../locale'
import { BatchActions, BatchTable } from '../components/BatchControls'
import FirmwareUrlInput from '../components/FirmwareUrlInput'

export default function BatchLoaderContent({ mode = 'browser' }: { mode?: 'browser' | 'desktop' }) {
  const { t } = usePublicLocale()
  const desktop = mode === 'desktop'
  return (
    <main className={`batch-main container${desktop ? ' batch-desktop' : ''}`} id="batch-loader" data-mode={mode}>
      <div className="batch-heading">
        <span className="section-label">
          {"TOOLS / H2LOADER"}
        </span>
        <h1>
          {t("批量刷机")}
        </h1>
        <p>
          {t(desktop ? "扫描电脑上的串口，校验固件包后批量发送或切换。设备能力与当前状态决定可执行操作。" : "授权 Web Serial 设备，校验固件包后批量发送或切换。设备能力与当前状态决定可执行操作。")}
        </p>
      </div>
      <div className="batch-notice" id="batch-notice" role="status" aria-live="polite">
        {t("正在启动 H2Loader…")}
      </div>
      <section className="batch-package" aria-labelledby="package-heading">
        <div>
          <span className="batch-kicker">
            {t("固件包")}
          </span>
          <h2 id="package-heading">
            {t("选择与校验")}
          </h2>
          <p>
            {t(desktop ? "固件包在本机校验和传输，不会上传到 h6infra Server。" : "包只在本页浏览器会话中使用，不会上传到 h6infra Server。")}
          </p>
        </div>
        <div className="batch-package-input">
          {desktop ? <Button variant="ghost" type="button" id="batch-package-choose" className="batch-file-label">{t("选择本地固件包")}</Button> : <label className="batch-file-label">
            {t("选择本地固件包 ")}
            <input id="batch-package-file" type="file" accept=".zlib,.tar.zlib,application/octet-stream" />
          </label>}
          <div className="batch-forge-actions">
            <Button variant="outline" type="button" id="batch-forge-picker">{t("从 Forge 选择 Release")}</Button>
            <Button variant="outline" type="button" id="batch-url-toggle" aria-expanded="false" aria-controls="batch-package-url-form">{t("从 URL 下载")}</Button>
            <Button variant="outline" type="button" id="batch-forge-cancel" hidden>{t("取消选择")}</Button>
          </div>
          <FirmwareUrlInput />
          <p className="batch-package-hint">
            {t("自动识别 App / Loader 包类型，无需预先选择。")}
          </p>
          <div className="batch-package-result" id="batch-package-result">
            {t("尚未选择固件包")}
          </div>
        </div>
      </section>
      <section className="batch-devices" aria-labelledby="devices-heading">
        <div className="batch-devices-head">
          <div>
            <span className="batch-kicker">
              {t("设备")}
            </span>
            <h2 id="devices-heading">
              {t(desktop ? "串口设备" : "已授权设备")}{' '}
              <small id="device-count">
                {t("0 台")}
              </small>
            </h2>
            <p>
              {t(desktop ? "扫描并读取设备身份与状态；显示系统串口路径，区分同型号设备。" : "授权后自动读取设备身份与状态；使用端口 ID 区分同型号设备。")}
            </p>
          </div>
          <div className="batch-discovery">
            {desktop ? <Button variant="ghost" type="button" id="scan-ports">
              {t("扫描串口")}
            </Button> : <Button variant="ghost" type="button" id="authorize-port">
              {t("授权端口")}
            </Button>}
          </div>
        </div>
        <BatchActions />
        <BatchTable desktop={desktop} />
        <p className="batch-footnote">
          {t(desktop ? "批量写操作最多同时处理 16 台设备。发送与切换只会在你点击操作后执行。" : "批量写操作最多同时处理 16 台设备。Web Serial 需要安全上下文以及 Chrome 或 Edge；发送与切换只会在你点击操作后执行。")}
        </p>
      </section>
    </main>
  )
}
