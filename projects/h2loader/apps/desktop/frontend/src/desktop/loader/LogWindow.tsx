import { invoke } from '@tauri-apps/api/core'
import { ArrowDownToLine, Cable, Circle, Download, ListFilter, Pause, Play, Search, Terminal, Trash2, X } from 'lucide-react'
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'
import { exportText, filterLogs, highlightParts, logTime, mergeLogs, shortPort, sourceLabels } from './log-model'
import type { LogPort, LogRecord, LogSnapshot } from './log-model'
import './logs.css'

const ROW_HEIGHT = 27

export default function LogWindow() {
  const [records, setRecords] = useState<LogRecord[]>([])
  const [ports, setPorts] = useState<LogPort[]>([])
  const [selectedPorts, setSelectedPorts] = useState<Set<string>>(() => new Set())
  const [keyword, setKeyword] = useState('')
  const [paused, setPaused] = useState<LogRecord[] | null>(null)
  const [following, setFollowing] = useState(true)
  const [dropped, setDropped] = useState(0)
  const [error, setError] = useState('')
  const [notice, setNotice] = useState('')
  const [busy, setBusy] = useState(false)
  const [scrollTop, setScrollTop] = useState(0)
  const [height, setHeight] = useState(500)
  const viewport = useRef<HTMLDivElement>(null)
  const cursor = useRef(0)
  const search = useRef<HTMLInputElement>(null)
  const selectionAnchor = useRef<string | null>(null)
  const shown = paused ?? records
  const filtered = useMemo(() => filterLogs(shown, selectedPorts, keyword), [shown, selectedPorts, keyword])
  const active = ports.filter(port => port.monitoring).length
  const pending = paused ? records.filter(record => record.id > (paused.at(-1)?.id ?? 0)).length : 0
  const start = Math.max(0, Math.floor(scrollTop / ROW_HEIGHT) - 8)
  const rows = filtered.slice(start, start + Math.ceil(height / ROW_HEIGHT) + 16)
  const longest = useMemo(() => filtered.reduce((max, record) => Math.max(max, record.message.length), 60), [filtered])
  const boards = useMemo(() => new Map(ports.map(item => [item.id, item.board])), [ports])
  const portLabel = (id: string) => `${boards.get(id) || 'Loader'} · ${shortPort(id)}`
  const selectedIds = [...selectedPorts]
  const selectionTitle = selectedIds.length === 1 ? portLabel(selectedIds[0]) : selectedIds.length ? `${selectedIds.length} 个串口` : '全部串口'

  useEffect(() => {
    let disposed = false
    let timer: ReturnType<typeof setTimeout>
    const poll = async () => {
      let delay = 250
      try {
        const snapshot = await invoke<LogSnapshot>('read_logs', { after: cursor.current })
        if (disposed) return
        cursor.current = snapshot.cursor
        setRecords(previous => mergeLogs(previous, snapshot))
        setPorts(snapshot.ports); setDropped(snapshot.dropped)
        if (snapshot.hasMore) delay = 0
      } catch (error) { if (!disposed) setError(String(error)) }
      if (!disposed) timer = setTimeout(poll, delay)
    }
    void poll()
    return () => { disposed = true; clearTimeout(timer) }
  }, [])
  useLayoutEffect(() => {
    const element = viewport.current!
    const observer = new ResizeObserver(() => setHeight(element.clientHeight))
    observer.observe(element)
    return () => observer.disconnect()
  }, [])
  useLayoutEffect(() => {
    if (following && !paused && viewport.current) viewport.current.scrollTop = viewport.current.scrollHeight
  }, [filtered, following, paused])
  useEffect(() => {
    const keydown = (event: KeyboardEvent) => {
      if ((event.metaKey || event.ctrlKey) && event.key === 'f') { event.preventDefault(); search.current?.focus() }
    }
    window.addEventListener('keydown', keydown)
    return () => window.removeEventListener('keydown', keydown)
  }, [])
  async function action(work: () => Promise<unknown>) {
    setBusy(true); setError(''); setNotice('')
    try { await work() } catch (error) { setError(String(error)) } finally { setBusy(false) }
  }
  function resetScroll() { setScrollTop(0); if (viewport.current) viewport.current.scrollTop = 0 }
  function selectPort(value: string, range = false) {
    const anchor = ports.findIndex(item => item.id === selectionAnchor.current)
    const target = ports.findIndex(item => item.id === value)
    setSelectedPorts(previous => {
      const next = new Set(previous)
      if (range && anchor >= 0 && target >= 0) {
        for (const item of ports.slice(Math.min(anchor, target), Math.max(anchor, target) + 1)) next.add(item.id)
      } else if (next.has(value)) next.delete(value)
      else next.add(value)
      return next
    })
    selectionAnchor.current = value
    resetScroll()
  }
  function selectAll() { setSelectedPorts(new Set()); selectionAnchor.current = null; resetScroll() }
  function resume() { setPaused(null); setFollowing(true) }
  return <main className="log-window">
    <header className="log-topbar">
      <div className="log-app-icon"><Terminal size={21} /></div>
      <div className="log-title"><h1>串口日志</h1><p>所有设备，一处查看</p></div>
      <span className="log-session-label">本次会话</span>
      <div className="log-top-actions">
        <button disabled={busy || !filtered.length} onClick={() => void action(async () => {
          const path = await invoke<string | null>('export_logs', { content: exportText(filtered) })
          if (path) setNotice(`已导出至 ${path}`)
        })}><Download size={15} />导出筛选结果</button>
        <button disabled={busy || !records.length} title="清空本次会话的全部串口日志" onClick={() => void action(async () => {
          await invoke('clear_logs'); setRecords([]); setPaused(null); setScrollTop(0); setDropped(0); setFollowing(true)
        })}><Trash2 size={15} />清空</button>
      </div>
    </header>
    <div className="log-body">
      <aside className="log-sidebar" aria-label="串口来源">
        <div className="log-sidebar-heading"><span>Loader 串口</span><span className="log-auto-label" title="直接点击可多选；按住 Shift 可连续选择">可多选</span></div>
        <button className="log-all-ports" aria-pressed={!selectedPorts.size} onClick={selectAll}><ListFilter size={17} /><span>全部串口</span><small>{ports.length}</small></button>
        <div className="log-port-list">
          {ports.map(item => <div key={item.id} className="log-port-item" data-selected={selectedPorts.has(item.id)}>
            <button className="log-port-select" aria-label={portLabel(item.id)} aria-pressed={selectedPorts.has(item.id)} title={`${item.id} · ${item.name}`} onClick={event => selectPort(item.id, event.shiftKey)}>
              <Cable size={12} /><span className="log-port-label"><strong>{item.board || 'Loader'}</strong><span>·</span><code>{shortPort(item.id).replace(/^usb(?:serial|modem)[-.]?(?=\d)/i, '')}</code></span>
            </button>

          </div>)}
          {!ports.length && <p className="log-sidebar-empty">请在主窗口扫描<br />并连接串口。</p>}
        </div>
        <div className="log-monitor-controls"><p>点击多选 · Shift 连选</p><p>与主窗口共用 Loader 连接</p></div>
      </aside>
      <section className="log-content" aria-label="日志记录">
        <div className="log-stream-heading"><div><h2>{selectionTitle}<span>{selectedIds.length === 1 ? '单设备' : selectedIds.length ? '多设备' : '聚合视图'}</span></h2><p>{selectedIds.length === 1 ? selectedIds[0] : selectedIds.length ? selectedIds.map(portLabel).join('、') : '按接收顺序排列 · 每条日志保留串口来源'}</p></div><span className="log-live-state" data-paused={!!paused}><Circle size={7} fill="currentColor" />{paused ? '显示已暂停' : '实时更新'}</span></div>
        <div className="log-filterbar">
          <div className="log-search"><Search size={16} /><input ref={search} aria-label="过滤关键词" placeholder="过滤关键词…" value={keyword} onChange={event => { setKeyword(event.target.value); setScrollTop(0); if (viewport.current) viewport.current.scrollTop = 0 }} />{keyword ? <button className="log-icon-button" aria-label="清除关键词" onClick={() => setKeyword('')}><X size={14} /></button> : <kbd>⌘ F</kbd>}</div>
          <button className="log-pause" aria-pressed={!!paused} onClick={() => paused ? resume() : setPaused([...records])}>{paused ? <Play size={14} /> : <Pause size={14} />}{paused ? '继续显示' : '暂停显示'}</button>
        </div>
        {(error || notice) && <div className="log-message-banner" role="status" data-error={!!error}>{error || notice}<button aria-label="关闭提示" className="log-icon-button" onClick={() => { setError(''); setNotice('') }}><X size={14} /></button></div>}
        <div className="log-console">
          <div className="log-columns"><span>时间</span><span>设备 / 串口</span><span>来源</span><span>内容</span></div>
          <div ref={viewport} className="log-viewport" tabIndex={0} aria-label="日志内容" onScroll={event => {
            const element = event.currentTarget; setScrollTop(element.scrollTop)
            setFollowing(element.scrollHeight - element.scrollTop - element.clientHeight < ROW_HEIGHT * 2)
          }}>
            {filtered.length ? <div className="log-virtual-space" role="table" aria-label="串口日志" aria-rowcount={filtered.length} style={{ height: filtered.length * ROW_HEIGHT, minWidth: Math.max(910, Math.min(60_000, longest * 13 + 455)) }}>
              <div style={{ transform: `translateY(${start * ROW_HEIGHT}px)` }}>{rows.map((record, rowIndex) => <div className="log-row" role="row" key={record.id} aria-rowindex={start + rowIndex + 1} data-source={record.source}>
                <time role="cell">{logTime(record.timestamp)}</time><span className="log-row-port" role="cell" title={`${portLabel(record.port)} · ${record.port}`} data-port={record.port} data-color={Math.abs([...record.port].reduce((value, char) => value + char.charCodeAt(0), 0)) % 5}>{portLabel(record.port)}</span><span role="cell" className="log-source">{sourceLabels[record.source] ?? record.source}</span><span role="cell" className="log-text">{highlightParts(record.message, keyword).map((part, index) => part.match ? <mark key={index}>{part.text}</mark> : part.text)}</span>
              </div>)}</div>
            </div> : <div className="log-empty"><Terminal size={34} /><h3>{keyword || selectedPorts.size ? '没有匹配的日志' : '等待第一条日志'}</h3><p>{keyword || selectedPorts.size ? '试试其他关键词，或切换到全部串口。' : '在主窗口连接串口后，会自动接收 Loader 日志。'}</p>{(keyword || selectedPorts.size > 0) && <button onClick={() => { setKeyword(''); selectAll() }}>重置筛选</button>}</div>}
          </div>
          {(!following || paused) && filtered.length > 0 && <button className="log-jump" onClick={resume}><ArrowDownToLine size={14} />{pending ? `${pending.toLocaleString()} 条新日志 · ` : ''}回到实时</button>}
        </div>
        <footer className="log-footer"><span>{filtered.length.toLocaleString()} 条{keyword || selectedPorts.size ? `匹配 / ${shown.length.toLocaleString()} 条记录` : '记录'}{active > 0 && ` · ${active} 个串口监听中`}</span><span>{dropped ? `已淘汰 ${dropped.toLocaleString()} 条旧日志 · ` : ''}最多保留 20,000 条 / 8 MiB</span></footer>
      </section>
    </div>
  </main>
}
