import React, {type ReactNode} from 'react';
import SiteMetadata from '@theme-original/SiteMetadata';
import Head from '@docusaurus/Head';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import {useLocation} from '@docusaurus/router';

// Once the hugr-lab.com copy of these docs exists, the GitHub Pages copy names
// it as the canonical page (customFields.canonicalBase, see docusaurus.config.ts).
// This Head comes after the original one, so its og:url and canonical win.
export default function SiteMetadataWrapper(): ReactNode {
  const {siteConfig: {baseUrl, customFields}} = useDocusaurusContext();
  const {pathname} = useLocation();
  const base = customFields?.canonicalBase as string | undefined;
  if (!base) {
    return <SiteMetadata />;
  }
  const url = base + (pathname.startsWith(baseUrl) ? pathname.slice(baseUrl.length) : '');
  return (
    <>
      <SiteMetadata />
      <Head>
        <meta property="og:url" content={url} />
        <link rel="canonical" href={url} />
      </Head>
    </>
  );
}
