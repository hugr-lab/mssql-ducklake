import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// Docs for mssql_ducklake - DuckLake on SQL Server. Mirrors the mssql-extension
// site (hugr-lab/mssql-extension/website) and the org site (hugr-lab.github.io)
// so the three share look and feel; published by this repo's Pages workflow to
// https://hugr-lab.github.io/mssql-ducklake/ (specs/013).

const config: Config = {
  title: 'DuckLake on SQL Server',
  tagline: 'A DuckDB extension that keeps a DuckLake catalog in Microsoft SQL Server - embedded DuckLake, native TDS.',
  favicon: 'img/favicon.ico',

  url: 'https://hugr-lab.github.io',
  baseUrl: '/mssql-ducklake/',
  trailingSlash: true,

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
          // VERSIONING CONTRACT: at each release, run
          //   npm run docusaurus docs:version <X.Y.Z>
          // in website/ and commit the snapshot. Docusaurus then serves the
          // latest RELEASED version at the root (the default) and the live
          // docs/ tree as "Next" under /next/ with an "unreleased" banner - so
          // in-progress docs for the coming release are reachable but never
          // the landing default. Until the first snapshot exists (v0.1.0,
          // specs/011), the current docs serve at the root and the version
          // dropdown has nothing to switch to.
          editUrl: 'https://github.com/hugr-lab/mssql-ducklake/tree/main/website/',
          showLastUpdateTime: true,
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themes: ['@docusaurus/theme-mermaid'],

  markdown: {
    mermaid: true,
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
          href: 'https://hugr-lab.github.io/',
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
            {label: 'Main site', href: 'https://hugr-lab.github.io/'},
            {label: 'DuckDB MSSQL Extension', href: 'https://hugr-lab.github.io/mssql-extension/'},
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
