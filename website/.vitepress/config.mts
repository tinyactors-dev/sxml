import { readFileSync } from 'node:fs'
import { defineConfig } from 'vitepress'

// The version the page shows is the library's, read from
// include/sxml/version.h at build time, so it is never typed twice.
function sxmlVersion(): string {
  const header = readFileSync(new URL('../../include/sxml/version.h', import.meta.url), 'utf8')
  const part = (name: string): string => {
    const match = header.match(new RegExp(`^#define SXML_VERSION_${name} (\\d+)$`, 'm'))
    if (match === null) {
      throw new Error(`include/sxml/version.h has no SXML_VERSION_${name}`)
    }
    return match[1]
  }
  return `${part('MAJOR')}.${part('MINOR')}.${part('PATCH')}`
}

// The site at sxml.tinyactors.dev: this landing page, with the mdbook
// book (docs/book) built into /book/ beside it (mise run site).
export default defineConfig({
  title: 'sxml',
  description:
    'An XML toolchain in portable C: XML and HTML parsing, XPath 4.0, XQuery 4.0, XSLT 4.0 and XSD 1.1 from one small library with no dependencies.',
  lang: 'en',
  cleanUrls: true,
  // The pipeline example's files (pipeline/AGENTS.md among them) are data, not pages.
  srcExclude: ['pipeline/**'],
  outDir: '../build/site',
  cacheDir: '../build/site-cache',
  head: [
    ['link', { rel: 'icon', type: 'image/svg+xml', href: '/favicon.svg' }],
    ['link', { rel: 'preload', href: '/fonts/Newsreader-400.woff2', as: 'font', type: 'font/woff2', crossorigin: '' }],
    ['link', { rel: 'preload', href: '/fonts/IBMPlexSans-400.woff2', as: 'font', type: 'font/woff2', crossorigin: '' }],
    ['meta', { property: 'og:title', content: 'sxml: XML, XPath, XQuery, XSLT and XSD in portable C' }],
    ['meta', { property: 'og:url', content: 'https://sxml.tinyactors.dev/' }],
  ],
  themeConfig: {
    sxmlVersion: sxmlVersion(),
    logo: undefined,
    siteTitle: 'sxml',
    // /book/ is a separate static site, so the router must not handle it.
    nav: [{ text: 'Book', link: '/book/', target: '_self' }],
    socialLinks: [{ icon: 'github', link: 'https://github.com/tinyactors-dev/sxml' }],
    footer: {
      message: 'Released under the MIT License.',
      copyright: 'sxml is a <a href="https://tinyactors.dev">tinyactors</a> project.',
    },
  },
})
