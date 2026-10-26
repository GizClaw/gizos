import { Channel, invoke, isTauri } from '@tauri-apps/api/core'
import { open } from '@tauri-apps/plugin-dialog'

interface Port { id: string; name?: string; usbVendorId?: number; usbProductId?: number }
interface Firmware { path?: string; id?: string; name?: string; size?: number; metadata?: InspectedPackage['metadata'] }
interface Progress { phase: string; progress: number; percent?: number; acknowledgedBytes?: number; totalBytes?: number }
interface Options { signal?: AbortSignal; onProgress?: (progress: Progress) => void }
interface InspectedPackage { id: string; name: string; size: number; metadata: { role: string; board: string; target: string; version: string } }
interface SelectionOptions { signal?: AbortSignal; onProgress?: (progress: { percent?: number | null; receivedBytes?: number; totalBytes?: number | null }) => void }

export async function createDesktopH2Loader() {
  if (!isTauri()) throw new Error('请在 H2Loader Desktop 桌面应用中打开此工作区。')
  await invoke('native_info')
  let closed = false
  const packageLeases = new Map<string, number>()
  const pendingRelease = new Set<string>()
  function releasePackage(file: Firmware) {
    if (!file?.id || closed) return
    const id = file.id
    if (packageLeases.get(id)) { pendingRelease.add(id); return }
    pendingRelease.delete(id)
    void invoke('release_package', { packageId: id }).catch(() => {})
  }
  function retainPackage(file: Firmware) {
    if (!file?.id) return () => {}
    const id = file.id
    packageLeases.set(id, (packageLeases.get(id) ?? 0) + 1)
    return () => {
      const remaining = (packageLeases.get(id) ?? 1) - 1
      if (remaining) packageLeases.set(id, remaining)
      else {
        packageLeases.delete(id)
        if (pendingRelease.has(id)) releasePackage({ id })
      }
    }
  }
  async function run(operation: string, port: Port, options: Options = {}, file?: Firmware) {
    if (closed) throw new Error('H2Loader 已关闭。')
    if (options.signal?.aborted) throw new DOMException('Aborted', 'AbortError')
    // Register before installing the abort listener, so early cancellation cannot
    // race ahead of the Rust job registry or the native serial process.
    const jobId = await invoke<string>('prepare_job', { portId: port.id, operation, packageId: file?.id ?? null })
    const cancel = () => { void invoke('cancel_job', { jobId }).catch(() => {}) }
    options.signal?.addEventListener('abort', cancel, { once: true })
    if (options.signal?.aborted) cancel()
    const progress = new Channel<Progress>()
    progress.onmessage = value => { if (!options.signal?.aborted && !closed) options.onProgress?.(value) }
    try { return await invoke('execute_job', { jobId, progress }) }
    catch (error) {
      if (options.signal?.aborted) throw new DOMException('Aborted', 'AbortError')
      throw new Error(String(error))
    } finally { options.signal?.removeEventListener('abort', cancel) }
  }
  async function selectPackage(command: string, args: Record<string, string>, { signal, onProgress }: SelectionOptions = {}) {
    if (closed) throw new Error('H2Loader 已关闭。')
    const selectionId = await invoke<string>('prepare_forge_picker')
    const cancel = () => { void invoke('cancel_forge_picker', { selectionId }).catch(() => {}) }
    signal?.addEventListener('abort', cancel, { once: true })
    if (signal?.aborted) cancel()
    const progress = new Channel<{ percent?: number | null; receivedBytes?: number; totalBytes?: number | null }>()
    progress.onmessage = value => { if (!signal?.aborted && !closed) onProgress?.(value) }
    try { return await invoke<InspectedPackage | null>(command, { selectionId, progress, ...args }) }
    finally { signal?.removeEventListener('abort', cancel) }
  }
  return {
    supported: () => true,
    releasePackage,
    retainPackage,
    getPorts: () => invoke<Port[]>('scan_ports'),
    getSavedPorts: () => invoke<Port[]>('remembered_ports'),
    readConnections: () => invoke<{ id: string; active: boolean; connected: boolean; status: unknown }[]>('connection_states'),
    connect: (port: Port, options?: Options) => run('connect', port, options),
    disconnect: (port: Port) => invoke('disconnect_port', { portId: port.id }),
    forgetPort: (port: Port) => invoke('forget_port', { portId: port.id }),
    choosePackage: async (): Promise<Firmware | null> => {
      const path = await open({ multiple: false, directory: false, filters: [{ name: 'H2Loader 固件包', extensions: ['zlib'] }] })
      return typeof path === 'string' ? { path } : null
    },
    inspectPackage: async (file: Firmware) => {
      if (file.metadata) return file.metadata
      const inspected = await invoke<InspectedPackage>('inspect_package', { path: file.path })
      Object.assign(file, { id: inspected.id, name: inspected.name, size: inspected.size })
      return inspected.metadata
    },
    chooseForgePackage: (options: SelectionOptions) => selectPackage('forge_package', {}, options),
    downloadPackage: (url: string, options?: SelectionOptions) => selectPackage('download_package', { url }, options),
    status: (port: Port, options?: Options) => run('status', port, options),
    stage: (port: Port, file: Firmware, options?: Options) => run('stage', port, options, file),
    rebootApp: (port: Port, options?: Options) => run('rebootApp', port, options),
    rebootLoader: (port: Port, options?: Options) => run('rebootLoader', port, options),
    rebootUpgrade: (port: Port, options?: Options) => run('rebootUpgrade', port, options),
    close: async () => { if (!closed) { closed = true; await invoke('close_client') } },
  }
}
