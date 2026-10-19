import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// Docs for mssql_ducklake - DuckLake on SQL Server. Mirrors the mssql-extension
// site (hugr-lab/mssql-extension/website) and the org site (hugr-lab.github.io)
// so the three share look and feel; published by this repo's Pages workflow to
// https://hugr-lab.github.io/mssql-ducklake/ (specs/013).

// The same docs are built twice: for GitHub Pages (default) and for hugr-lab.com on IONOS (DOCS_TARGET=ionos).
const ionos = process.env.DOCS_TARGET === 'ionos';
const site = ionos
  ? {url: 'https://hugr-lab.com', baseUrl: '/docs/mssql-ducklake/', home: 'https://hugr-lab.com/', impressum: 'https://hugr-lab.com/impressum/', datenschutz: 'https://hugr-lab.com/datenschutz/'}
  : {url: 'https://hugr-lab.github.io', baseUrl: '/mssql-ducklake/', home: 'https://hugr-lab.github.io/', impressum: 'https://hugr-lab.github.io/impressum/', datenschutz: 'https://hugr-lab.github.io/datenschutz/'};
// While the hugr-lab.com copy exists (DOCS_IONOS on), pages.yml sets DOCS_CANONICAL for
// the GitHub Pages build: its pages then name the hugr-lab.com copy as canonical
// (src/theme/SiteMetadata) and it has no sitemap of its own.
const canonicalBase = ionos ? '' : process.env.DOCS_CANONICAL ?? '';

const config: Config = {
  title: 'DuckLake on SQL Server',
  tagline: 'A DuckDB extension that keeps a DuckLake catalog in Microsoft SQL Server - embedded DuckLake, native TDS.',
  favicon: 'img/favicon.ico',

  url: site.url,
  baseUrl: site.baseUrl,
  baseUrlIssueBanner: false,
  trailingSlash: true,
  customFields: {docsTarget: ionos ? 'ionos' : 'github', canonicalBase},

  organizationName: 'hugr-lab',
  projectName: 'mssql-ducklake',

  // 'throw', not 'warn': docs-build.yml is the PR gate for website/, and a
  // gate that exits 0 on a broken link does not gate anything.
  onBrokenLinks: 'throw',
  onBrokenAnchors: 'throw',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: './sidebars.ts',
          // Docs ARE the site: /mssql-ducklake/<page>/
          routeBasePath: '/',
          // VERSIONING CONTRACT: the versions are made at DEPLOY time, from
          // the release tags - pages.yml runs `docusaurus docs:version <X.Y.Z>`
          // on each tag's own tree (specs/011), so nothing is committed for
          // them. Docusaurus then serves the latest RELEASED version at the
          // root (the default) and the live docs/ tree as "Next" under /next/
          // with an "unreleased" banner - so in-progress docs for the coming
          // release are reachable but never the landing default.
          editUrl: 'https://github.com/hugr-lab/mssql-ducklake/tree/main/website/',
          showLastUpdateTime: true,
        },
        blog: false,
        sitemap: canonicalBase ? false : undefined,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themes: ['@docusaurus/theme-mermaid'],

  markdown: {
    mermaid: true,
    // The hugr-lab.com copy shows no image from another server - the released versions' badges
    // included: their docs come from the tags, so removing them from docs/ is not enough.
    preprocessor: ({fileContent}) =>
      ionos
        ? fileContent.replace(/\[!\[[^\]]*\]\(https?:\/\/[^)]*\)\]\([^)]*\)|!\[[^\]]*\]\(https?:\/\/[^)]*\)/g, '')
        : fileContent,
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
  },

  themeConfig: {
    metadata: [
      {name: 'keywords', content: 'DuckDB, DuckLake, SQL Server, Azure SQL, MSSQL, lakehouse, catalog, extension, TDS'},
      {name: 'description', content: 'DuckDB extension that embeds DuckLake and keeps its catalog in Microsoft SQL Server or Azure SQL through the mssql extension.'},
    ],
    navbar: {
      title: 'mssql_ducklake',
      logo: {
        alt: 'Hugr Lab',
        src: 'img/logo-circle.svg',
        href: '/',
      },
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'docsSidebar',
          position: 'left',
          label: 'Docs',
        },
        {
          type: 'docsVersionDropdown',
          position: 'right',
        },
        {
          href: 'https://hugr-lab.github.io/mssql-extension/',
          label: 'MSSQL Extension',
          position: 'left',
        },
        {
          href: site.home,
          label: 'Hugr Lab',
          position: 'right',
        },
        {
          href: 'https://github.com/hugr-lab/mssql-ducklake',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    colorMode: {
      defaultMode: 'light',
      disableSwitch: true,
      respectPrefersColorScheme: false,
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Docs',
          items: [
            {label: 'Getting Started', to: '/getting-started/'},
            {label: 'The catalog in SQL Server', to: '/catalog/'},
            {label: 'Performance', to: '/performance/'},
            {label: 'Limitations', to: '/reference/limitations/'},
          ],
        },
        {
          title: 'Community',
          items: [
            {label: 'GitHub', href: 'https://github.com/hugr-lab/mssql-ducklake'},
            {label: 'Issues', href: 'https://github.com/hugr-lab/mssql-ducklake/issues'},
            {label: 'DuckLake', href: 'https://ducklake.select'},
            {label: 'DuckDB Community Extensions', href: 'https://duckdb.org/community_extensions/'},
          ],
        },
        {
          title: 'Hugr Lab',
          items: [
            {label: 'Main site', href: site.home},
            {label: 'DuckDB MSSQL Extension', href: 'https://hugr-lab.github.io/mssql-extension/'},
          ],
        },
        {
          title: 'Legal',
          items: [
            {label: 'Impressum', href: site.impressum},
            {label: 'Datenschutz', href: site.datenschutz},
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} Hugr Lab.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['sql', 'bash'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
