import { expect, test } from '@playwright/test'

// IPC fixture exercises the real desktop adapter without opening hardware.
test('desktop uses native scanning, actual port paths and only the inner workbench', async ({ page }, testInfo) => {
  const requested: string[] = []
  page.on('request', request => requested.push(request.url()))
  await page.addInitScript(() => {
    let sequence = 0
    const callbacks = new Map<number, (value: unknown) => void>()
    const jobs = new Map<string, { operation: string; port?: string; cancelled?: boolean; resolve?: () => void; reject?: (error: Error) => void }>()
    const connected = new Set<string>()
    const fixture = { now: 0, portCount: 1, calls: [] as string[], releases: [] as string[], staged: [] as string[], finish: () => { for (const job of [...jobs.values()]) job.resolve?.() }, report: (_bytes: number, _at: number) => {} }
    const status = { board: 'test_board', target: 'esp32s3', active: { role: 'app', version: '1.2.3' }, commandAvailability: (1 << 0) | (1 << 1) | (1 << 3) | (1 << 11) | (1 << 19) }
    Object.assign(globalThis, {
      isTauri: true,
      __desktopTest: fixture,
      __TAURI_INTERNALS__: {
        transformCallback: (callback: (value: unknown) => void) => { callbacks.set(++sequence, callback); return sequence },
        unregisterCallback: (id: number) => callbacks.delete(id),
        invoke: async (command: string, args: Record<string, unknown>) => {
          fixture.calls.push(command)
          if (command === 'native_info') return { transport: 'native' }
          if (command === 'remembered_ports') return []
          if (command === 'connection_states') return [...connected].map(id => ({ id, connected: true, active: true, status }))
          if (command === 'disconnect_port') { connected.delete(String(args.portId)); return null }
          if (command === 'open_log_window') return null
          if (command === 'scan_ports') return Array.from({ length: fixture.portCount }, (_, i) => ({ id: `/dev/cu.usbmodem-test${i ? `-${i}` : ''}`, name: 'USB Serial', usbVendorId: 0x303a, usbProductId: 0x1001, selectedByDefault: true }))
          if (command === 'plugin:dialog|open') return '/fixtures/app.tar.zlib'
          if (command === 'inspect_package') return { id: 'package-1', name: 'app.tar.zlib', size: 2097152, metadata: { role: 'app', board: 'test_board', target: 'esp32s3', version: '1.2.4' } }
          if (command === 'prepare_forge_picker') return 'forge-1'
          if (command === 'forge_package') return { id: 'package-forge', name: 'forge-loader.tar.zlib', size: 2097152, metadata: { role: 'h2loader', board: 'test_board', target: 'esp32s3', version: '1.2.5' } }
          if (command === 'download_package') return { id: 'package-url', name: 'url-app.tar.zlib', size: 2097152, metadata: { role: 'app', board: 'test_board', target: 'esp32s3', version: '1.2.6' } }
          if (command === 'release_package') { fixture.releases.push(String(args.packageId)); return null }
          if (command === 'prepare_job') { const id = `job-${++sequence}`; jobs.set(id, { operation: String(args.operation), port: String(args.portId) }); if (args.operation === 'stage') fixture.staged.push(String(args.packageId)); return id }
          if (command === 'cancel_job') { const job = jobs.get(String(args.jobId)); if (job) { jobs.delete(String(args.jobId)); job.cancelled = true; job.reject?.(new Error('Cancelled')) }; return null }
          if (command === 'close_client') return null
          if (command === 'execute_job') {
            const job = jobs.get(String(args.jobId))!
            if (job.cancelled) throw new Error('Cancelled')
            if (job.operation === 'status' || job.operation === 'connect') { connected.add(job.port!); jobs.delete(String(args.jobId)); return status }
            const progress = args.progress as { onmessage: (value: unknown) => void }
            return new Promise((resolve, reject) => {
              job.resolve = () => { jobs.delete(String(args.jobId)); resolve(status) }
              job.reject = reject
              fixture.report = (bytes, at) => { fixture.now = at; progress.onmessage({ phase: 'stage', progress: bytes * 100 / 2097152, totalBytes: 2097152, acknowledgedBytes: bytes }) }
              progress.onmessage({ phase: 'connect', progress: 0 })
            })
          }
          throw new Error(`Unexpected IPC: ${command}`)
        },
      },
    })
    Object.defineProperty(performance, 'now', { configurable: true, value: () => fixture.now })
    Object.defineProperty(navigator, 'serial', { configurable: true, get: () => { throw new Error('Desktop must not access Web Serial') } })
  })
  await page.goto('/desktop/loader.html')
  await expect(page.locator('#scan-ports')).toBeEnabled()
  await page.getByRole('button', { name: '串口日志', exact: true }).click()
  await expect(page.locator('#authorize-port')).toHaveCount(0)
  await expect(page.locator('.public-site-header, .footer')).toHaveCount(0)
  await expect(page.locator('.desktop-toolbar')).toHaveCount(1)
  await expect(page.locator('input[name="package-role"]')).toHaveCount(0)
  await page.locator('.desktop-firmware-menu summary').click()
  await page.locator('#batch-forge-picker').click()
  await expect(page.locator('.desktop-package-name')).toHaveText('forge-loader.tar.zlib')
  await expect(page.locator('.desktop-package-role')).toHaveText('Loader')
  await expect(page.locator('#batch-firmware-menu-label')).toHaveText('选择固件')
  await expect(page.locator('#device-count')).toHaveText('0 台')
  await page.locator('#scan-ports').click()
  await expect(page.locator('#device-rows')).toContainText('/dev/cu.usbmodem-test')
  await expect(page.locator('#device-rows .batch-pill')).toHaveText('未连接')
  await expect(page.locator('input[type=checkbox]')).toHaveCount(0)
  await page.locator('#connect-ports').click()
  await expect(page.locator('#device-rows .batch-pill')).toHaveText('已连接')
  await expect(page.getByRole('button', { name: '移除授权' })).toHaveCount(0)
  await page.locator('.desktop-firmware-menu summary').click()
  await page.locator('#batch-package-choose').click()
  await expect(page.locator('.desktop-package-name')).toHaveText('app.tar.zlib')
  await expect(page.locator('.desktop-package-role')).toHaveText('App')
  await page.locator('[data-operation="stage"]').click()
  await expect(page.locator('.batch-task')).toContainText('正在连接')
  expect(await page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { releases: string[] } }).__desktopTest.releases)).not.toContain('package-1')
  await expect(page.locator('#batch-forge-picker')).toBeEnabled()
  await page.locator('.desktop-firmware-menu summary').click()
  await page.locator('#batch-url-toggle').click()
  await page.locator('#batch-package-url').fill('https://example.com/url-app.tar.zlib')
  await page.locator('#batch-url-download').click()
  await expect(page.locator('.desktop-package-name')).toHaveText('url-app.tar.zlib')
  await expect(page.locator('.desktop-package-version')).toHaveText('版本 1.2.6')
  await expect(page.locator('.desktop-package-verified')).toHaveText('已校验')
  await expect(page.locator('#batch-firmware-menu-label')).toHaveText('选择固件')
  await expect(page.locator('.batch-task')).toContainText('正在连接')
  expect(await page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { releases: string[] } }).__desktopTest.releases)).not.toContain('package-1')
  await page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { report: (bytes: number, at: number) => void } }).__desktopTest.report(0, 5000))
  await page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { report: (bytes: number, at: number) => void } }).__desktopTest.report(1048576, 6000))
  await expect(page.locator('.batch-transfer-rate')).toHaveText('1.0 MiB/s')
  await expect(page.locator('.batch-task-percent')).toHaveText('50%')
  for (const width of [800, 1024, 1440]) {
    await page.setViewportSize({ width, height: 1000 })
    await expect.poll(() => page.locator('#scan-ports').evaluate(element => element.getBoundingClientRect().right <= document.documentElement.clientWidth)).toBe(true)
    const layout = await page.evaluate(() => {
      const name = document.querySelector('.desktop-package-name')!.getBoundingClientRect()
      const metadata = document.querySelector('.desktop-package-meta')!.getBoundingClientRect()
      const scan = document.querySelector('#scan-ports')!.getBoundingClientRect()
      const choose = document.querySelector('.desktop-firmware-menu')!.getBoundingClientRect()
      const tableContainer = document.querySelector('.batch-table-scroll')!
      return { nameBottom: name.bottom, metadataTop: metadata.top, scanRight: scan.right, scanLeft: scan.left, chooseRight: choose.right,
        viewportWidth: document.documentElement.clientWidth,
        tableOverflow: getComputedStyle(tableContainer).overflow }
    })
    expect(await page.locator('.desktop-workbench').evaluate(element => element.getBoundingClientRect().width)).toBeGreaterThanOrEqual(Math.max(layout.viewportWidth, 1150))
    expect(layout.metadataTop).toBeGreaterThanOrEqual(layout.nameBottom)
    expect(layout.scanRight).toBeLessThanOrEqual(layout.viewportWidth)
    expect(layout.chooseRight).toBeLessThan(layout.scanLeft)
    expect(layout.scanLeft - layout.chooseRight).toBeLessThanOrEqual(15)
    expect(layout.tableOverflow).toBe('visible')
    if (width < 1150) {
      await page.evaluate(() => window.scrollTo({ left: 150, behavior: 'instant' }))
      expect(await page.evaluate(() => window.scrollX)).toBeGreaterThan(0)
      expect(await page.locator('.batch-table-scroll').evaluate(element => element.scrollLeft)).toBe(0)
      await expect(page.locator('#scan-ports')).toBeInViewport()
      await page.evaluate(() => window.scrollTo(0, 0))
    }
  }
  await page.screenshot({ path: testInfo.outputPath('desktop-workbench.png'), fullPage: true })
  await page.locator('#cancel-all').click()
  await expect(page.locator('.batch-task')).toContainText('已取消')
  await expect(page.locator('.batch-transfer-rate')).toHaveCount(0)
  await expect.poll(() => page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { releases: string[] } }).__desktopTest.releases)).toContain('package-1')
  await expect(page.locator('#connect-ports')).toHaveText('断开串口')
  await page.locator('#connect-ports').click()
  await expect(page.locator('#desktop-connection-count')).toHaveText('已连接 0 台')
  await page.evaluate(() => { (globalThis as typeof globalThis & { __desktopTest: { portCount: number } }).__desktopTest.portCount = 18 })
  await page.setViewportSize({ width: 1440, height: 600 })
  await page.locator('#scan-ports').click()
  await expect(page.locator('#device-rows tr')).toHaveCount(18)
  await page.locator('#connect-ports').click()
  await expect(page.locator('.batch-pill[data-state=connected]')).toHaveCount(18)
  expect(await page.evaluate(() => document.scrollingElement!.scrollHeight > innerHeight)).toBe(true)
  expect(await page.locator('.batch-table-scroll').evaluate(element => element.scrollHeight === element.clientHeight)).toBe(true)
  await page.evaluate(() => window.scrollTo(0, document.body.scrollHeight))
  await expect(page.locator('.desktop-statusbar')).toBeInViewport()
  await expect(page.locator('.desktop-toolbar')).toBeInViewport()
  await page.locator('[data-operation="stage"]').click()
  await expect(page.locator('.batch-pill[data-state="queued"]')).toHaveCount(2)
  await page.locator('.desktop-firmware-menu summary').click()
  await page.locator('#batch-package-choose').click()
  await expect(page.locator('.desktop-package-name')).toHaveText('app.tar.zlib')
  expect(await page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { releases: string[] } }).__desktopTest.releases)).not.toContain('package-url')
  await page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { finish: () => void } }).__desktopTest.finish())
  await expect(page.locator('.batch-pill[data-state="busy"]')).toHaveCount(2)
  const staged = await page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { staged: string[] } }).__desktopTest.staged)
  expect(staged.slice(1)).toEqual(Array.from({ length: 18 }, () => 'package-url'))
  await page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { finish: () => void } }).__desktopTest.finish())
  await expect.poll(() => page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { releases: string[] } }).__desktopTest.releases)).toContain('package-url')
  const calls = await page.evaluate(() => (globalThis as typeof globalThis & { __desktopTest: { calls: string[] } }).__desktopTest.calls)
  expect(calls).toContain('scan_ports')
  expect(calls).toContain('open_log_window')
  expect(calls).toContain('cancel_job')
  expect(calls).toContain('plugin:dialog|open')
  expect(calls).toContain('forge_package')
  expect(calls).toContain('download_package')
  expect(requested.filter(url => /h2loader_runtime.*\.wasm/.test(url))).toEqual([])
})

test('desktop connects and disconnects the list, restores it on startup and scans every available port', async ({ page }, testInfo) => {
  await page.addInitScript(() => {
    let sequence = 0
    const inventory = ['/dev/cu.usbmodem-a', '/dev/cu.usbserial-b']
    const saved: string[] = JSON.parse(localStorage.getItem('fixture-saved') ?? '[]')
    const removed: string[] = JSON.parse(localStorage.getItem('fixture-removed') ?? '[]')
    const connections = new Map<string, number>()
    const jobs = new Map<string, { operation: string; port: string }>()
    const status = { deviceUid: '001122334455', board: 'fixture', target: 'esp32s3', active: { role: 'app', version: '1.2.3' }, commandAvailability: 1 << 0 }
    const fixture = { requests: [] as { operation: string; port: string; connection: number }[] }
    const persist = () => {
      localStorage.setItem('fixture-saved', JSON.stringify(saved))
      localStorage.setItem('fixture-removed', JSON.stringify(removed))
    }
    const ports = (add: boolean) => {
      if (add) {
        removed.splice(0)
        for (const id of inventory) if (!saved.includes(id)) saved.push(id)
      }
      persist()
      return saved.filter(id => !removed.includes(id)).map(id => ({ id, name: 'USB Serial', present: true, remembered: true, connected: connections.has(id), active: connections.has(id), initialStatus: connections.has(id) ? status : null }))
    }
    Object.assign(globalThis, {
      isTauri: true, __connectionTest: fixture,
      __TAURI_INTERNALS__: {
        transformCallback: () => ++sequence,
        unregisterCallback: () => {},
        invoke: async (command: string, args: Record<string, unknown>) => {
          if (command === 'native_info') return { transport: 'native' }
          if (command === 'remembered_ports') return ports(false)
          if (command === 'scan_ports') return ports(true)
          if (command === 'connection_states') return [...connections].map(([id]) => ({ id, active: true, connected: true, status }))
          if (command === 'disconnect_port' || command === 'forget_port') {
            const id = String(args.portId)
            connections.delete(id)
            if (command === 'forget_port') { removed.push(id); persist() }
            return null
          }
          if (command === 'prepare_job') {
            const id = `job-${++sequence}`
            jobs.set(id, { operation: String(args.operation), port: String(args.portId) })
            return id
          }
          if (command === 'execute_job') {
            const job = jobs.get(String(args.jobId))!
            if (job.operation === 'connect') connections.set(job.port, ++sequence)
            if (!connections.has(job.port)) throw new Error('Connect first')
            fixture.requests.push({ ...job, connection: connections.get(job.port)! })
            jobs.delete(String(args.jobId))
            return status
          }
          if (command === 'close_client') { connections.clear(); return null }
          throw new Error(`Unexpected IPC: ${command}`)
        },
      },
    })
  })
  const requests = () => page.evaluate(() => (globalThis as typeof globalThis & { __connectionTest: { requests: { operation: string; port: string; connection: number }[] } }).__connectionTest.requests)
  const row = (id: string) => page.locator('#device-rows tr').filter({ hasText: id })
  const a = '/dev/cu.usbmodem-a', b = '/dev/cu.usbserial-b'
  await page.goto('/desktop/loader.html')
  await expect(page.locator('#scan-ports')).toBeEnabled()
  await expect(page.locator('#connect-ports')).toBeDisabled()
  await page.locator('#scan-ports').click()
  await expect(page.locator('#device-rows tr')).toHaveCount(2)
  await expect(page.locator('input[type=checkbox]')).toHaveCount(0)
  await expect(page.locator('[data-operation=status]')).toHaveText('刷新状态')
  await expect(page.locator('[data-operation=status]')).toBeDisabled()
  expect(await requests()).toEqual([])
  await row(a).getByRole('button', { name: '连接', exact: true }).click()
  await expect(row(a).locator('.batch-pill')).toHaveText('已连接')
  await expect(row(b).locator('.batch-pill')).toHaveText('未连接')
  await expect(page.locator('#connect-ports')).toHaveText('断开串口')
  await row(b).getByRole('button', { name: '连接', exact: true }).click()
  await expect(page.locator('#desktop-connection-count')).toHaveText('已连接 2 台')
  const initial = await requests()
  expect(initial.map(item => item.port)).toEqual([a, b])
  await page.locator('[data-operation=status]').click()
  await expect.poll(async () => (await requests()).length).toBe(4)
  expect((await requests()).slice(2)).toEqual(initial.map(item => ({ ...item, operation: 'status' })))
  await page.locator('#scan-ports').click()
  await expect(page.locator('#desktop-connection-count')).toHaveText('已连接 2 台')
  expect((await requests()).length).toBe(4)
  await row(a).getByRole('button', { name: '断开', exact: true }).click()
  await expect(row(a).locator('.batch-pill')).toHaveText('未连接')
  await page.locator('[data-operation=status]').click()
  await expect.poll(async () => (await requests()).length).toBe(5)
  expect((await requests())[4]).toEqual({ ...initial[1], operation: 'status' })
  await row(b).getByRole('button', { name: '移除', exact: true }).click()
  await expect(page.locator('#device-rows tr')).toHaveCount(1)
  await expect(page.locator('#desktop-connection-count')).toHaveText('已连接 0 台')
  await page.reload()
  await expect(page.locator('#device-rows tr')).toHaveCount(1)
  await expect(row(a)).toContainText('上次列表')
  await expect(row(a).locator('.batch-pill')).toHaveText('未连接')
  expect(await requests()).toEqual([])
  await expect(page.locator('#restore-ports, .desktop-port-menu')).toHaveCount(0)
  await page.locator('#scan-ports').click()
  await expect(page.locator('#device-rows tr')).toHaveCount(2)
  expect(await requests()).toEqual([])
  await page.locator('#connect-ports').click()
  await expect(page.locator('#desktop-connection-count')).toHaveText('已连接 2 台')
  await expect(page.locator('#connect-ports')).toHaveText('断开串口')
  await page.locator('#connect-ports').click()
  await expect(page.locator('#desktop-connection-count')).toHaveText('已连接 0 台')
  await expect(page.locator('[data-operation=status]')).toBeDisabled()
  await expect(page.locator('#connect-ports')).toHaveText('连接串口')
  await page.locator('#connect-ports').click()
  await expect(page.locator('#desktop-connection-count')).toHaveText('已连接 2 台')
  await page.screenshot({ path: testInfo.outputPath('desktop-connections.png'), fullPage: true })
})

test('superseded native inspection releases its snapshot and keeps the latest selection', async ({ page }) => {
  await page.addInitScript(() => {
    let choice = 0
    const fixture = { finishFirst: () => {}, releases: [] as string[] }
    const result = (id: string) => ({ id, name: `${id}.tar.zlib`, size: 123, metadata: { role: 'app', board: 'test_board', target: 'esp32s3', version: '1.0.0' } })
    Object.assign(globalThis, { isTauri: true, __snapshotTest: fixture,
      __TAURI_INTERNALS__: { invoke: async (command: string, args: Record<string, unknown>) => {
        if (command === 'native_info' || command === 'close_client') return null
        if (command === 'remembered_ports' || command === 'connection_states') return []
        if (command === 'plugin:dialog|open') return ++choice === 1 ? '/first.tar.zlib' : '/latest.tar.zlib'
        if (command === 'release_package') { fixture.releases.push(String(args.packageId)); return null }
        if (command === 'inspect_package') {
          if (args.path === '/first.tar.zlib') return new Promise(resolve => { fixture.finishFirst = () => resolve(result('first')) })
          return result('latest')
        }
        throw new Error(`Unexpected IPC ${command}`)
      } },
    })
  })
  await page.goto('/desktop/loader.html')
  await expect(page.locator('#batch-package-choose')).toBeEnabled()
  await page.locator('.desktop-firmware-menu summary').click()
  await page.locator('#batch-package-choose').click()
  await expect(page.locator('#batch-package-result')).toContainText('正在校验')
  await page.locator('.desktop-firmware-menu summary').click()
  await page.locator('#batch-package-choose').click()
  await expect(page.locator('#batch-package-result')).toContainText('latest.tar.zlib')
  await page.evaluate(() => (globalThis as typeof globalThis & { __snapshotTest: { finishFirst: () => void } }).__snapshotTest.finishFirst())
  await expect.poll(() => page.evaluate(() => (globalThis as typeof globalThis & { __snapshotTest: { releases: string[] } }).__snapshotTest.releases)).toEqual(['first'])
  await expect(page.locator('#batch-package-result')).toContainText('latest.tar.zlib')
})

test('independent logs aggregate ports, filter literally, pause, export and stay bounded', async ({ page }, testInfo) => {
  await page.addInitScript(() => {
    let sequence = 0
    const ports = [
      { id: '/dev/cu.usbserial-210', board: 'H106', name: 'H106 · App', present: true, monitoring: true, status: 'listening' },
      { id: '/dev/cu.usbmodem-101', board: 'devkit', name: 'devkit · Loader', present: true, monitoring: true, status: 'listening' },
      { id: '/dev/cu.usbserial-310', board: 'bk7258_v3_202405', name: 'bk7258_v3_202405 · App', present: true, monitoring: true, status: 'listening' },
    ]
    const fixture = { records: [] as { id: number; timestamp: number; port: string; source: string; message: string }[], cleared: 0, calls: [] as string[], exported: '', append: (port: string, message: string, source = 'device') => { fixture.records.push({ id: ++sequence, timestamp: 1791453600000 + sequence * 137, port, source, message }) } }
    const lines = ['[boot] GizOS started · firmware v1.6.20', '[WiFi] Connecting to lab-network', '[WiFi] Connected · RSSI -42 dBm', '[app] H106 ready', '[audio] Output device initialized', '[sensor] temperature=26.4°C', 'Literal .* and <script> remain text']
    for (let i = 0; i < 36; i++) fixture.append(ports[i % 3].id, lines[i % lines.length], i % 9 === 0 ? 'system' : 'device')
    Object.assign(globalThis, { isTauri: true, __logTest: fixture, __TAURI_INTERNALS__: { invoke: async (command: string, args: Record<string, unknown>) => {
      fixture.calls.push(command)
      if (command === 'read_logs') {
        const records = fixture.records.filter(record => record.id > Number(args.after)).slice(0, 1000)
        const cursor = records.at(-1)?.id ?? sequence
        return { records, ports, cursor, oldest: fixture.records[0]?.id ?? sequence + 1, clearedThrough: fixture.cleared, dropped: 0, hasMore: cursor < sequence }
      }
      if (command === 'clear_logs') { fixture.records = []; fixture.cleared = sequence; return null }

      if (command === 'export_logs') { fixture.exported = String(args.content); return '/fixtures/logs.txt' }
      throw new Error(`Log window must not create or close the workbench client: ${command}`)
    } } })
  })
  await page.setViewportSize({ width: 1180, height: 760 })
  await page.goto('/desktop/loader.html?view=logs')
  await expect(page.locator('.log-footer')).toContainText('36 条记录')
  await expect(page.locator('.desktop-workbench')).toHaveCount(0)
  await expect(page.locator('.log-row-port').filter({ hasText: 'usbserial-210' }).first()).toBeVisible()
  await expect(page.locator('.log-row-port').filter({ hasText: 'usbserial-210' }).first()).toHaveText('H106 · usbserial-210')
  await expect(page.locator('.log-port-select').first()).toContainText('H106·210')
  await expect(page.locator('.log-port-select').last()).toHaveAccessibleName('bk7258_v3_202405 · usbserial-310')
  await expect(page.locator('.log-port-select').last()).toHaveAttribute('title', '/dev/cu.usbserial-310 · bk7258_v3_202405 · App')
  await page.screenshot({ path: testInfo.outputPath('serial-logs-window.png') })
  await page.locator('.log-port-select').first().click()
  await page.getByRole('textbox', { name: '过滤关键词' }).fill('wifi')
  await expect(page.locator('.log-footer')).toContainText('匹配')
  await expect(page.locator('.log-text mark').first()).toHaveText('WiFi')
  const selected = (await page.locator('.log-port-select[aria-pressed=true]').getAttribute('title'))!.split(' · ')[0]
  for (const value of await page.locator('.log-row-port').evaluateAll(elements => elements.map(element => element.getAttribute('data-port')))) expect(value).toBe(selected)
  await expect(page.locator('.log-stream-heading h2')).toContainText('H106 · usbserial-210')
  await page.getByRole('button', { name: '导出筛选结果' }).click()
  const exported = await page.evaluate(() => (globalThis as typeof globalThis & { __logTest: { exported: string } }).__logTest.exported)
  expect(exported.toLowerCase()).toContain('wifi')
  expect(exported.split('\n').filter(Boolean).every(line => line.includes(selected!))).toBe(true)
  await page.locator('.log-port-select').nth(1).click()
  await expect(page.locator('.log-port-select[aria-pressed=true]')).toHaveCount(2)
  await expect(page.locator('.log-stream-heading h2')).toContainText('2 个串口')
  const selectedPair = ['/dev/cu.usbserial-210', '/dev/cu.usbmodem-101']
  for (const value of await page.locator('.log-row-port').evaluateAll(elements => elements.map(element => element.getAttribute('data-port')))) expect(selectedPair).toContain(value)
  await page.getByRole('button', { name: '导出筛选结果' }).click()
  const pairExport = await page.evaluate(() => (globalThis as typeof globalThis & { __logTest: { exported: string } }).__logTest.exported)
  expect(pairExport).toContain(selectedPair[0])
  expect(pairExport).toContain(selectedPair[1])
  expect(pairExport).not.toContain('/dev/cu.usbserial-310')
  await page.locator('.log-port-select').first().click()
  await expect(page.locator('.log-port-select[aria-pressed=true]')).toHaveCount(1)
  await expect(page.locator('.log-stream-heading h2')).toContainText('devkit · usbmodem-101')
  await page.getByRole('button', { name: '全部串口' }).click()
  await page.locator('.log-port-select').first().click()
  await page.locator('.log-port-select').last().click({ modifiers: ['Shift'] })
  await expect(page.locator('.log-port-select[aria-pressed=true]')).toHaveCount(3)
  await expect(page.locator('.log-stream-heading h2')).toContainText('3 个串口')
  await page.locator('.log-port-select').last().hover()
  expect(await page.locator('.log-port-select').last().evaluate(element => getComputedStyle(element).backgroundColor)).toBe('rgba(0, 0, 0, 0)')
  await page.getByRole('button', { name: '清除关键词' }).click()
  await page.getByRole('button', { name: '全部串口' }).click()
  await page.getByRole('textbox', { name: '过滤关键词' }).fill('.*')
  await expect(page.locator('.log-text mark').first()).toHaveText('.*')
  await expect(page.locator('.log-text').first()).toContainText('<script>')
  await expect(page.locator('.log-text script')).toHaveCount(0)
  await page.getByRole('button', { name: '清除关键词' }).click()
  await page.getByRole('button', { name: '暂停显示' }).click()
  await page.evaluate(() => (globalThis as typeof globalThis & { __logTest: { append: (port: string, message: string) => void } }).__logTest.append('/dev/cu.usbserial-210', 'PAUSED_ARRIVAL'))
  await expect(page.getByRole('button', { name: /1 条新日志/ })).toBeVisible()
  await expect(page.locator('.log-text').filter({ hasText: 'PAUSED_ARRIVAL' })).toHaveCount(0)
  await page.getByRole('button', { name: '继续显示' }).click()
  await expect(page.locator('.log-text').filter({ hasText: 'PAUSED_ARRIVAL' })).toBeVisible()
  await expect(page.getByRole('button', { name: /监听|扫描串口/ })).toHaveCount(0)
  expect(await page.locator('.log-sidebar').evaluate(element => element.getBoundingClientRect().width)).toBeLessThanOrEqual(170)
  expect(await page.locator('.log-port-item').first().evaluate(element => element.getBoundingClientRect().height)).toBeLessThanOrEqual(32)
  await expect(page.locator('.log-port-state')).toHaveCount(0)
  await page.evaluate(() => { const fixture = (globalThis as typeof globalThis & { __logTest: { append: (port: string, message: string) => void } }).__logTest; for (let i = 0; i < 18_000; i++) fixture.append('/dev/cu.usbserial-210', `stress ${i}`) })
  await expect(page.locator('.log-footer')).toContainText('18,037 条记录')
  expect(await page.locator('.log-row').count()).toBeLessThan(100)
  await page.getByRole('button', { name: '清空', exact: true }).click()
  await expect(page.locator('.log-empty h3')).toHaveText('等待第一条日志')
  await page.evaluate(() => (globalThis as typeof globalThis & { __logTest: { append: (port: string, message: string) => void } }).__logTest.append('/dev/cu.usbserial-210', 'AFTER_CLEAR'))
  await expect(page.locator('.log-footer')).toContainText('1 条记录')
  await expect(page.locator('.log-text')).toHaveText('AFTER_CLEAR')
  await page.reload()
  await expect(page.locator('.log-footer')).toContainText('36 条记录')
  // This fixture reload resets the simulated backend; real Rust history has its
  // own bounded-history and monotonic-clear tests and outlives a webview.
})
