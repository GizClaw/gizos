import { useEffect, useRef, type ReactNode } from 'react'
import { mountBatchLoader } from './batch-loader/batch.js'
const t = (source: string) => source
export default function BatchLoaderWorkspace({createClient, children}: {mode: 'desktop'; createClient: () => Promise<unknown>; children: ReactNode}) {
  const root = useRef<HTMLDivElement>(null)
  useEffect(() => {
    let cleanup: (() => void) | undefined
    const start = () => { if (!cleanup && root.current) cleanup = mountBatchLoader(root.current, {language: 'zh-CN', t, mode: 'desktop', createClient}) }
    const stop = () => { cleanup?.(); cleanup = undefined }
    start(); window.addEventListener('pagehide', stop); window.addEventListener('pageshow', start)
    return () => { stop(); window.removeEventListener('pagehide', stop); window.removeEventListener('pageshow', start) }
  }, [createClient])
  return <div ref={root}>{children}</div>
}
