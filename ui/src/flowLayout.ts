import { Choice, FlowEdge, FlowGraph, FlowNode, Parameter } from './types';

export interface PlacedNode extends FlowNode { x: number; y: number; width: number; height: number }
export interface Layout { nodes: PlacedNode[]; edges: FlowEdge[]; width: number; height: number }
const WIDTH = 206, COLUMN = 290, GAP = 28, MARGIN = 44;

// The compiler owns topology. This pass only places its named signals on screen.
export function layoutFlow(graph: FlowGraph, parameters: Map<string, Parameter>, choices: Map<string, Choice> = new Map(), fontScale = 1): Layout {
  const ids = new Set(graph.nodes.map(node => node.id));
  const byId = new Map(graph.nodes.map(node => [node.id, node]));
  const authoredOrder = new Map(graph.nodes.map((node, index) => [node.id, index]));
  const edges = graph.edges.filter(edge => ids.has(edge.source) && ids.has(edge.target) && edge.source !== edge.target);
  const incoming = new Map<string, string[]>(), outgoing = new Map<string, string[]>();
  for (const node of graph.nodes) { incoming.set(node.id, []); outgoing.set(node.id, []); }
  for (const edge of edges) { incoming.get(edge.target)!.push(edge.source); outgoing.get(edge.source)!.push(edge.target); }
  const indegree = new Map([...incoming].map(([id, values]) => [id, values.length]));
  const queue = graph.nodes.filter(node => !indegree.get(node.id)).map(node => node.id);
  const rank = new Map(graph.nodes.map(node => [node.id, 0]));
  const order: string[] = [];
  for (let at = 0; at < queue.length; ++at) {
    const id = queue[at]; order.push(id);
    for (const target of outgoing.get(id) || []) {
      rank.set(target, Math.max(rank.get(target)!, rank.get(id)! + 1));
      indegree.set(target, indegree.get(target)! - 1);
      if (indegree.get(target) === 0) queue.push(target);
    }
  }
  // Delay short control branches towards their consumer instead of stacking
  // every envelope next to the input event. Inputs keep their left-hand anchor.
  for (const id of [...order].reverse()) {
    const node = byId.get(id)!;
    const targets = outgoing.get(id)!;
    if (targets.length && !node.signal.startsWith('key.') && node.kind === 'control')
      rank.set(id, Math.max(rank.get(id)!, Math.min(...targets.map(target => rank.get(target)!)) - 1));
  }
  const columns = new Map<number, FlowNode[]>();
  for (const node of graph.nodes) {
    const column = rank.get(node.id) || 0;
    columns.set(column, [...columns.get(column) || [], node]);
  }
  const heights = new Map(graph.nodes.map(node => {
    const choiceCount = (node.choiceIds || []).filter(id => choices.has(id)).length;
    const parameterCount = node.parameterIds.filter(id => parameters.has(id)).length;
    const compact = node.signal.startsWith('key.') && !choiceCount && !parameterCount;
    // Each choice occupies a 26px row, a 4px row gap and 8px vertical padding.
    const height = (compact ? 64 : parameterCount ? 174 : 108) + (choiceCount ? choiceCount * 30 + 12 : 0);
    return [node.id, height] as const;
  }));
  const totalHeight = Math.max(220, ...[...columns.values()].map(nodes => nodes.reduce((sum, node) => sum + heights.get(node.id)! + GAP, -GAP)));
  const row = new Map<string, number>();
  for (const [column, nodes] of [...columns].sort(([a], [b]) => a - b)) {
    nodes.sort((a, b) => {
      const center = (node: FlowNode) => {
        const parents = incoming.get(node.id)!.filter(id => row.has(id));
        return parents.length ? parents.reduce((sum, id) => sum + row.get(id)!, 0) / parents.length : authoredOrder.get(node.id)!;
      };
      // Keep the audible chain on the top lane; its control sources sit below.
      const lane = (node: FlowNode) => node.kind === 'audio' ? 0 : node.kind === 'control' ? 1 : 2;
      return lane(a) - lane(b) || center(a) - center(b);
    });
    nodes.forEach((node, index) => row.set(node.id, index)); columns.set(column, nodes);
  }
  const nodes: PlacedNode[] = [];
  for (const [column, values] of columns) {
    let y = MARGIN;
    for (const node of values) {
      const height = heights.get(node.id)!;
      nodes.push({ ...node, x: MARGIN + column * COLUMN, y, width: WIDTH, height });
      y += height + GAP;
    }
  }
  // Scale geometry with text so cards and their ports keep matching at every size.
  const scale = Number.isFinite(fontScale) ? Math.max(.625, Math.min(2, fontScale)) : 1;
  return { nodes: nodes.map(node => ({ ...node, x: node.x * scale, y: node.y * scale, width: node.width * scale, height: node.height * scale })), edges,
    width: (MARGIN * 2 + Math.max(0, ...rank.values()) * COLUMN + WIDTH) * scale, height: (totalHeight + MARGIN * 2) * scale };
}

export function pathToNode(graph: FlowGraph, selected: string | null): Set<string> {
  if (!selected) return new Set();
  const active = new Set<string>([selected]);
  for (const direction of ['source', 'target'] as const) {
    const adjacent = new Map<string, string[]>();
    for (const edge of graph.edges) {
      const from = direction === 'source' ? edge.target : edge.source;
      const to = direction === 'source' ? edge.source : edge.target;
      adjacent.set(from, [...adjacent.get(from) || [], to]);
    }
    const seen = new Set<string>([selected]), pending = [selected];
    for (let i = 0; i < pending.length; ++i) for (const to of adjacent.get(pending[i]) || [])
      if (!seen.has(to)) { seen.add(to); active.add(to); pending.push(to); }
  }
  return active;
}

// Keep authored signals, but omit unused event inputs (including unused gate,
// pitch and trigger). An explicit probe creates an edge and keeps its input.
export function visibleFlowGraph(graph: FlowGraph): FlowGraph {
  const connected = new Set(graph.edges.flatMap(edge => [edge.source, edge.target]));
  return { ...graph, nodes: graph.nodes.filter(node => !node.signal.startsWith('key.') || connected.has(node.id) || node.id === graph.output) };
}
