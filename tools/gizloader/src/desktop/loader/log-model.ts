export interface LogRecord { id: number; timestamp: number; port: string; source: string; message: string }
export interface LogPort { id: string; name: string; board: string; present: boolean; monitoring: boolean; status: string }
export interface LogSnapshot { records: LogRecord[]; ports: LogPort[]; cursor: number; oldest: number; clearedThrough: number; dropped: number; hasMore: boolean }
export const sourceLabels: Record<string, string> = { device: '设备', stdout: '任务', stderr: '诊断', system: '系统' }
export function exportText(records: LogRecord[]) {
  return records.map(record => `${new Date(record.timestamp).toISOString()}\t${record.port}\t${record.source}\t${record.message}\n`).join('')
}
export function mergeLogs(previous: LogRecord[], snapshot: LogSnapshot): LogRecord[] {
  const retained = previous.filter(record => record.id >= snapshot.oldest && record.id > snapshot.clearedThrough)
  const last = retained.at(-1)?.id ?? 0
  return [...retained, ...snapshot.records.filter(record => record.id > last && record.id > snapshot.clearedThrough)].slice(-20_000)
}
export function filterLogs(records: LogRecord[], ports: ReadonlySet<string>, keyword: string) {
  const needle = keyword.toLowerCase()
  return records.filter(record => (!ports.size || ports.has(record.port)) && record.message.toLowerCase().includes(needle))
}
export function shortPort(port: string) { return port.replace(/^\/dev\/(?:cu\.|tty\.)/, '') }
export function logTime(timestamp: number) {
  const date = new Date(timestamp)
  return `${date.toLocaleTimeString('en-GB', { hour12: false })}.${String(date.getMilliseconds()).padStart(3, '0')}`
}
export function highlightParts(message: string, keyword: string): { text: string; match: boolean }[] {
  if (!keyword) return [{ text: message, match: false }]
  const parts = []; const haystack = message.toLowerCase(); const needle = keyword.toLowerCase()
  let start = 0; let found: number
  while ((found = haystack.indexOf(needle, start)) !== -1) {
    if (found > start) parts.push({ text: message.slice(start, found), match: false })
    parts.push({ text: message.slice(found, found + keyword.length), match: true }); start = found + keyword.length
  }
  if (start < message.length) parts.push({ text: message.slice(start), match: false })
  return parts
}
