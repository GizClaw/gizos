import test from 'node:test'
import assert from 'node:assert/strict'
import { mergeLogs, filterLogs, highlightParts, exportText } from '../src/desktop/loader/log-model.ts'
const record = (id, port = 'COM1', message = `line ${id}`) => ({ id, port, message, source: 'device', timestamp: 1791444000000 + id })
const snapshot = (records, rest = {}) => ({ records, ports: [], oldest: 1, clearedThrough: 0, dropped: 0, cursor: records.at(-1)?.id ?? 0, hasMore: false, ...rest })

test('cursor history deduplicates, evicts, clears and bounds multi-port records', () => {
  const records = [record(1), record(2, 'COM2'), record(3)]
  assert.deepEqual(mergeLogs(records, snapshot([record(3), record(4)], { oldest: 2 })).map(r => r.id), [2, 3, 4])
  assert.deepEqual(mergeLogs(records, snapshot([record(5)], { clearedThrough: 4, oldest: 5 })).map(r => r.id), [5])
  assert.equal(mergeLogs([], snapshot(Array.from({ length: 20_010 }, (_, i) => record(i + 1)))).length, 20_000)
})
test('port and case-insensitive literal keyword compose without regex or HTML interpretation', () => {
  const records = [record(1, 'COM1', 'Ready .* <script>'), record(2, 'COM2', 'READY .*'), record(3, 'COM1', 'Ready')]
  assert.deepEqual(filterLogs(records, new Set(['COM1']), '.*').map(r => r.id), [1])
  assert.equal(filterLogs(records, new Set(), 'ready').length, 3)
  assert.deepEqual(filterLogs(records, new Set(['COM1', 'COM2']), '.*').map(r => r.id), [1, 2])
  assert.deepEqual(highlightParts('a.*b.*', '.*'), [{ text: 'a', match: false }, { text: '.*', match: true }, { text: 'b', match: false }, { text: '.*', match: true }])
  assert.equal(highlightParts('<script>你好</script>', '你好')[1].text, '你好')
})
test('export uses the exact frozen filtered records and retains date and port', () => {
  const frozen = [record(1, 'COM2', '中文日志')]
  const content = exportText(filterLogs(frozen, new Set(['COM2', 'COM3']), '中文'))
  assert.match(content, /T.*Z\tCOM2\tdevice\t中文日志\n$/)
  assert.equal(exportText(filterLogs(frozen, new Set(['COM1']), '')), '')
})
