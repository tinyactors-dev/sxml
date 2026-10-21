import { defineConfig } from 'vitepress'

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
