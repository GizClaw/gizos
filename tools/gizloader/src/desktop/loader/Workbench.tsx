import { ChevronDown, DownloadCloud, FileArchive, FolderOpen, Link2, RefreshCw, Terminal, Cable } from 'lucide-react'
import { useLayoutEffect, useRef, useState } from 'react'
import { invoke } from '@tauri-apps/api/core'
import { Button } from '@/components/ui/button'
import { BatchActions, BatchTable } from '../../public/components/BatchControls'
import FirmwareUrlInput from '../../public/components/FirmwareUrlInput'

export default function DesktopWorkbench() {
  const workbench = useRef<HTMLElement>(null)
  const [openingLogs, setOpeningLogs] = useState(false)
  const [logError, setLogError] = useState('')
  useLayoutEffect(() => {
    // Window controls fit the viewport even when the document has scrollbars.
    const fitControls = () => workbench.current?.style.setProperty('--desktop-viewport-width', `${document.documentElement.clientWidth}px`)
    const observer = new ResizeObserver(fitControls)
    observer.observe(document.documentElement)
    window.addEventListener('resize', fitControls)
    fitControls()
    return () => { observer.disconnect(); window.removeEventListener('resize', fitControls) }
  }, [])
  return <main ref={workbench} className="desktop-workbench" id="batch-loader" data-mode="desktop">
    <header className="desktop-toolbar">
      <FileArchive className="desktop-firmware-icon" aria-hidden="true" />
      <div className="desktop-firmware-copy" aria-label="当前固件包"><div id="batch-package-result" className="batch-package-result">尚未选择固件包</div></div>
      <div className="desktop-titlebar-space" data-tauri-drag-region />
      <Button variant="ghost" id="open-serial-logs" disabled={openingLogs} onClick={async () => {
        setOpeningLogs(true); setLogError('')
        try { await invoke('open_log_window') } catch (error) { setLogError(String(error)) } finally { setOpeningLogs(false) }
      }}><Terminal aria-hidden="true" />串口日志</Button>
      <Button variant="ghost" id="connect-ports" data-operation="connect"><Cable aria-hidden="true" /><span id="connection-action-label">连接串口</span></Button>
      <Button variant="ghost" id="batch-forge-cancel" hidden>取消选择</Button>
      <details className="desktop-firmware-menu" id="batch-firmware-menu" onClick={event => {
        if ((event.target as HTMLElement).closest('button')) event.currentTarget.removeAttribute('open')
      }}>
        <summary><span id="batch-firmware-menu-label">选择固件</span><ChevronDown aria-hidden="true" /></summary>
        <div className="desktop-file-actions" aria-label="固件来源">
          <Button variant="ghost" id="batch-package-choose"><FolderOpen aria-hidden="true" />本地文件</Button>
          <Button variant="ghost" id="batch-url-toggle" aria-expanded="false" aria-controls="batch-package-url-form"><Link2 aria-hidden="true" />URL</Button>
          <Button variant="ghost" id="batch-forge-picker"><DownloadCloud aria-hidden="true" />Forge</Button>
        </div>
      </details>
      <Button variant="ghost" id="scan-ports"><RefreshCw aria-hidden="true" />扫描串口</Button>

    </header>
    {logError && <div role="alert" className="desktop-log-error">{logError}</div>}
    <FirmwareUrlInput />
    <section className="batch-devices desktop-devices" aria-labelledby="devices-heading">
      <BatchActions desktop />
      <div className="batch-notice desktop-notice" id="batch-notice" role="status" aria-live="polite">正在启动 Gizloader…</div>
      <div className="desktop-table-heading"><h2 id="devices-heading">设备列表</h2><small id="device-count">0 台</small><small id="desktop-connection-count">已连接 0 台</small></div>
      <BatchTable desktop />
    </section>
    <footer className="desktop-statusbar"><span>本机串口 · 最多同时处理 16 台设备</span><span>连接串口并选择固件后执行发送</span></footer>
  </main>
}
