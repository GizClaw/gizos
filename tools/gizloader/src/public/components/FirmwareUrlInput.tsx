import { Button } from '@/components/ui/button'
import { usePublicLocale } from '../locale'

export default function FirmwareUrlInput() {
  const { t } = usePublicLocale()
  return <form id="batch-package-url-form" className="batch-url-form" hidden aria-label={t('固件包 URL')}>
    <label htmlFor="batch-package-url">{t('固件包 URL')}</label>
    <input id="batch-package-url" type="url" required placeholder="https://…/firmware.tar.zlib" autoComplete="off" spellCheck={false} />
    <Button type="submit" variant="ghost" id="batch-url-download">{t('下载并校验')}</Button>
    <Button type="button" variant="ghost" id="batch-url-close">{t('收起')}</Button>
  </form>
}
