import { useMemo, type ReactNode } from 'react'
import { LocaleContext, localizedHref, translate, type PublicLanguage } from './locale'

export function PublicLocaleProvider({ language, children }: { language: PublicLanguage; children: ReactNode }) {
  const value = useMemo(() => ({
    language,
    t: (source: string) => translate(source, language),
    href: (source: string) => localizedHref(source, language),
  }), [language])
  return <LocaleContext.Provider value={value}>{children}</LocaleContext.Provider>
}
