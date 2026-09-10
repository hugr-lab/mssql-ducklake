import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

const sidebars: SidebarsConfig = {
  docsSidebar: [
    'index',
    'getting-started',
    {
      type: 'category',
      label: 'The catalog in SQL Server',
      link: {type: 'doc', id: 'catalog/index'},
      items: ['catalog/attach', 'catalog/shaping', 'catalog/requirements'],
    },
    'writing',
    'performance',
    {
      type: 'category',
      label: 'Reference',
      items: ['reference/settings', 'reference/limitations', 'reference/troubleshooting'],
    },
    'versions',
    'releases',
    'development',
  ],
};

export default sidebars;
