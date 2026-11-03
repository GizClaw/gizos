import { useEffect, useRef, type ReactNode } from 'react'
import BatchLoaderContent from './pages/BatchLoaderContent'
import { mountBatchLoader } from './batch-loader/batch.js'
import { usePublicLocale } from './locale'

export default function BatchLoaderWorkspace({ mode, createClient, children }: {
  mode: 'browser' | 'desktop'
  createClient: () => Promise<unknown>
  children?: ReactNode
}) {
  const root = useRef<HTMLDivElement>(null)
  const { language, t } = usePublicLocale()
  useEffect(() => {
    let disposed = false
    let active = true
    let cleanup: (() => void) | undefined
    const start = () => {
      if (!disposed && active && !cleanup && root.current) cleanup = mountBatchLoader(root.current, { language, t, mode, createClient })
    }
    const hide = () => { active = false; cleanup?.(); cleanup = undefined }
    const show = () => { active = true; start() }
    window.addEventListener('pagehide', hide)
    window.addEventListener('pageshow', show)
    start()
    return () => {
      disposed = true
      hide()
      window.removeEventListener('pagehide', hide)
      window.removeEventListener('pageshow', show)
    }
  }, [language, t, mode, createClient])
  return <div ref={root}>{children ?? <BatchLoaderContent mode={mode} />}</div>
}
