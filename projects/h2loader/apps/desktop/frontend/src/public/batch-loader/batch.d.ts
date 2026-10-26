export function mountBatchLoader(
  root: HTMLElement,
  options: { language: string; t: (source: string) => string; mode: 'browser' | 'desktop'; createClient: () => Promise<unknown> },
): () => void
