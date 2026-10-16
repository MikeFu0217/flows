import { useEffect, useMemo, useState } from 'react';
import { Icon } from './Icon';
import { countInstruments, ExplorerFile, ExplorerNode, instrumentTree, parentFolders } from './explorerTree';
import './Explorer.css';
import type { SampleAsset } from './types';

export function Explorer({ files, query, activePath, connected, workspaceId, samples = [], onImportSample, onSampleInstrument, onImportFiles, onOpen }: {
  onImportFiles: (files: File[], folder: string) => Promise<void>;
  samples?: SampleAsset[]; onImportSample: () => void; onSampleInstrument: (path: string) => void;
  files: ExplorerFile[]; query: string; activePath: string; connected: boolean; workspaceId?: string; onOpen: (path: string) => void;
}) {
  const [selectedSample, setSelectedSample] = useState('');
  const [dropFolder, setDropFolder] = useState('');
  const [samplesOpen, setSamplesOpen] = useState(true);
  const matchingSamples = samples.filter(sample => sample.path.toLowerCase().includes(query.trim().toLowerCase()));
  const selected = samples.find(sample => sample.path === selectedSample);
  const tree = useMemo(() => instrumentTree([...files, ...samples.map(sample => ({ path: sample.path, name: sample.name }))], query), [files, samples, query]);
  const [expanded, setExpanded] = useState<Set<string>>(() => new Set(parentFolders(activePath)));
  useEffect(() => { setExpanded(new Set(parentFolders(activePath))); }, [workspaceId]);
  useEffect(() => {
    setExpanded(previous => new Set([...previous, ...parentFolders(activePath)]));
  }, [activePath]);
  const toggle = (path: string) => setExpanded(previous => {
    const next = new Set(previous); if (next.has(path)) next.delete(path); else next.add(path); return next;
  });
  const render = (nodes: ExplorerNode[], depth: number) => nodes.map(node => {
    if (node.kind === 'folder') {
      const open = !!query.trim() || expanded.has(node.path);
      return <div key={node.path} className={`explorer-folder ${dropFolder === node.path ? 'drop-target' : ''}`}
        onDragOver={event => { if (connected && event.dataTransfer.types.includes('Files')) { event.preventDefault(); event.stopPropagation(); event.dataTransfer.dropEffect = 'copy'; setDropFolder(node.path); } }}
        onDragLeave={event => { if (!event.currentTarget.contains(event.relatedTarget as Node)) setDropFolder(''); }}
        onDrop={event => { if (!event.dataTransfer.files.length) return; event.preventDefault(); event.stopPropagation(); setDropFolder(''); setExpanded(value => new Set([...value, node.path])); void onImportFiles(Array.from(event.dataTransfer.files), node.path); }}>
        <button className="folder-entry" style={{ paddingLeft: 8 + depth * 13 }} aria-expanded={open} title={node.path} onClick={() => toggle(node.path)}>
          <Icon name={open ? 'down' : 'chevron'} size={12}/><Icon name="folder" size={14}/><span>{node.name}</span>
        </button>
        {open && <div className="folder-children">{render(node.children, depth + 1)}</div>}
      </div>;
    }
    const sample = samples.find(item => item.path === node.path);
    return <button draggable={!!sample} onDragStart={event => { if (!sample) return; event.dataTransfer.setData('application/flows-sample', sample.path); event.dataTransfer.setData('text/plain', JSON.stringify(sample.path)); event.dataTransfer.effectAllowed = 'copy'; }} key={node.path} className={`file-entry ${activePath === node.path ? 'active' : ''}`} style={{ paddingLeft: 10 + depth * 13 }}
      title={node.path} aria-current={activePath === node.path ? 'page' : undefined} disabled={!connected} onClick={() => sample ? setSelectedSample(sample.path) : onOpen(node.path)}>
      <Icon name={sample ? "sound" : "file"} size={14}/><span>{node.name}</span>{activePath === node.path && <i/>}
    </button>;
  });
  return <div className="explorer-content"><div className="tree-root"><Icon name="down" size={12}/><Icon name="folder" size={14}/><span>Instruments</span><small>{countInstruments(tree)}</small></div>
    <nav className="file-tree" aria-label="Instruments">{render(tree, 0)}{!tree.length && <p className="files-empty">{connected ? 'No matching instruments.' : 'Your instruments appear here when connected to the native host.'}</p>}</nav>
    <div className="tree-root sample-heading"><button className="sample-toggle" aria-expanded={samplesOpen} onClick={() => setSamplesOpen(value => !value)}><Icon name={samplesOpen ? 'down' : 'chevron'} size={12}/><Icon name="sound" size={14}/><span>Samples</span></button><small>{matchingSamples.length}</small><button className="icon-button" title="Import audio sample" aria-label="Import audio sample" disabled={!connected} onClick={onImportSample}><Icon name="add" size={14}/></button></div>
    {samplesOpen && <nav className="sample-tree" aria-label="Samples">{matchingSamples.map(sample => <button key={sample.path} className={`file-entry sample-entry ${selectedSample === sample.path ? 'active' : ''}`} disabled={!connected}
      title={`${sample.path} · ${sample.duration.toFixed(2)} s · ${sample.channels === 1 ? 'Mono' : 'Stereo'} · drag into code to insert its path`}
      draggable onDragStart={event => { event.dataTransfer.setData('application/flows-sample', sample.path); event.dataTransfer.setData('text/plain', JSON.stringify(sample.path)); event.dataTransfer.effectAllowed = 'copy'; }} onClick={() => setSelectedSample(sample.path)}>
      <Icon name="sound" size={14}/><span>{sample.name}</span><small>{sample.duration.toFixed(1)}s</small></button>)}
      {!matchingSamples.length && <p className="files-empty">{samples.length ? 'No matching samples.' : 'Import audio to play it, process it and inspect it in code.'}</p>}
      {selected && <div className="sample-details"><code title={selected.path}>{selected.path}</code><span>{selected.channels === 1 ? 'Mono' : 'Stereo'} · {(selected.sampleRate / 1000).toFixed(1)} kHz · {selected.duration.toFixed(2)} s</span><button className="secondary-button" disabled={!connected} onClick={() => onSampleInstrument(selected.path)}><Icon name="add" size={13}/> New sampler instrument</button><p>Drag the sample into a <code>file:</code> argument to use it in your code.</p></div>}
    </nav>}
  </div>;
}
