import { createContext, useContext } from 'react'
import englishText from './locales/en.json'

export type PublicLanguage = 'zh-CN' | 'en'
const translations: Record<string, string> = englishText

export function translate(source: string, language: PublicLanguage) {
  return language === 'en' ? translations[source] ?? source : source
}

export function localizedHref(href: string, language: PublicLanguage) {
  if (!href.startsWith('/') || href.startsWith('//')) return href
  const base = href.replace(/^\/en(?=\/)/, '')
  const pathname = base.split(/[?#]/)[0]
  if (/^\/(api|public|admin)(\/|$)/.test(pathname)) return href
  const publicPage = pathname === '/' || pathname === '/play' || pathname === '/play/' || pathname === '/tools/' || pathname.endsWith('.html')
  return publicPage ? `${language === 'en' ? '/en' : ''}${base}` : href
}

export function languagePath(language: PublicLanguage, pathname: string, search = '', hash = '') {
  const base = pathname.replace(/^\/en(?=\/)/, '')
  return `${language === 'en' ? '/en' : ''}${base}${search}${hash}`
}

interface PublicLocale {
  language: PublicLanguage
  t: (source: string) => string
  href: (source: string) => string
}
export const LocaleContext = createContext<PublicLocale>({
  language: 'zh-CN', t: source => source, href: source => source,
})

export function usePublicLocale() { return useContext(LocaleContext) }
