import { createRoot } from 'react-dom/client'
import BatchLoaderWorkspace from '../../public/BatchLoaderWorkspace'
import { PublicLocaleProvider } from '../../public/PublicLocaleProvider'
import { createDesktopH2Loader } from './client'
import DesktopWorkbench from './Workbench'
import LogWindow from './LogWindow'
import './base.css'
import './styles.css'

createRoot(document.getElementById('root')!).render(new URLSearchParams(window.location.search).get('view') === 'logs' ? <LogWindow /> :
  <PublicLocaleProvider language="zh-CN">
    <BatchLoaderWorkspace mode="desktop" createClient={createDesktopH2Loader}><DesktopWorkbench /></BatchLoaderWorkspace>
  </PublicLocaleProvider>,
)
