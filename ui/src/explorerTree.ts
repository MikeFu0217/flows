export interface ExplorerFile { path: string; name?: string; kind?: string }
export interface ExplorerNode { path: string; name: string; kind: 'folder' | 'file'; children: ExplorerNode[] }

/** Build folders from the native host's workspace-relative paths. */
export function instrumentTree(files: ExplorerFile[], query = ''): ExplorerNode[] {
  const roots: ExplorerNode[] = [], seen = new Set<string>();
  const needle = query.trim().toLowerCase();
  for (const file of files) {
    if (seen.has(file.path)) continue;
    const parts = file.path.split(/[\\/]/).filter(Boolean);
    if (!parts.length || parts.some(part => part === '.' || part === '..')) continue;
    if (needle && !`${file.path} ${file.name || ''}`.toLowerCase().includes(needle)) continue;
    seen.add(file.path);
    let siblings = roots;
    for (let index = 0; index < parts.length; ++index) {
      const leaf = index === parts.length - 1 && file.kind !== 'directory';
      const path = leaf ? file.path : parts.slice(0, index + 1).join('/');
      let node = siblings.find(item => item.path === path && item.kind === (leaf ? 'file' : 'folder'));
      if (!node) {
        node = { path, name: leaf ? file.name || parts[index] : parts[index], kind: leaf ? 'file' : 'folder', children: [] };
        siblings.push(node);
      }
      siblings = node.children;
    }
  }
  const sort = (nodes: ExplorerNode[]) => {
    nodes.sort((a, b) => a.kind === b.kind ? a.name.localeCompare(b.name, 'en', { numeric: true }) : a.kind === 'folder' ? -1 : 1);
    nodes.forEach(node => sort(node.children));
  };
  sort(roots);
  return roots;
}

export function parentFolders(path: string): string[] {
  const parts = path.split(/[\\/]/).filter(Boolean);
  return parts.slice(0, -1).map((_, index) => parts.slice(0, index + 1).join('/'));
}

export function countInstruments(nodes: ExplorerNode[]): number {
  return nodes.reduce((sum, node) => sum + (node.kind === 'file' ? 1 : countInstruments(node.children)), 0);
}
