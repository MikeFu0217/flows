import type { Document, Snapshot } from './types';

export type Draft = { text: string; documentId: string; baseRevision: number; gestureId?: string; gesturePhase?: string; transactionId: string; conflict?: boolean };
type Transport = (method: string, params?: Record<string, unknown>) => Promise<any>;

/** A document owns its draft; the one audio engine changes instruments only
 * after the outgoing source has been acknowledged. Every source/selection
 * operation shares this queue, including edits typed during an activation. */
export class DocumentSession {
  snapshot: Snapshot | null = null;
  readonly drafts = new Map<string, Draft>();
  private tail: Promise<unknown> = Promise.resolve();
  private applying?: Draft;
  private sequence = 0;
  pendingOperations = 0;
  constructor(private transport: Transport, private changed: () => void) {}
  documents() { return this.snapshot?.documents ?? (this.snapshot?.document.id ? [{ ...this.snapshot.document, compile: this.snapshot.compile }] : []); }
  document(id: string) { return this.documents().find(document => document.id === id); }
  text(id: string) { return this.drafts.get(id)?.text ?? this.document(id)?.text ?? ''; }
  dirty(id: string) { const document = this.document(id); return !!this.drafts.has(id) || !!document && (document.dirty ?? document.revision !== document.savedRevision); }
  receive(value: any) {
    const incoming = value?.snapshot ?? value;
    if (!incoming?.document || typeof incoming.document.text !== 'string') return;
    const previous = this.snapshot;
    const next = { ...previous, ...incoming, files: incoming.files ?? previous?.files ?? [], compile: incoming.compile ?? previous?.compile ?? { status: 'idle' }, audio: incoming.audio ?? previous?.audio ?? {}, ai: incoming.ai ?? previous?.ai ?? {} } as Snapshot;
    if (previous?.sessionId === next.sessionId && next.document.revision < previous.document.revision) return;
    if (previous && previous.sessionId !== next.sessionId) this.drafts.forEach(draft => { draft.conflict = true; });
    this.snapshot = next;
    for (const [id, draft] of this.drafts) {
      const document = this.document(id);
      if (document && document.revision !== draft.baseRevision && this.applying?.documentId !== id) {
        const old = previous?.documents?.find(value => value.id === id) ?? (previous?.document.id === id ? previous.document : undefined);
        if (old?.text === document.text || document.text === draft.text) draft.baseRevision = document.revision;
        else draft.conflict = true;
      }
    }
    this.changed();
  }
  change(id: string, text: string, gestureId?: string, gesturePhase?: string) {
    const document = this.document(id); if (!document) return;
    const previous = this.drafts.get(id);
    this.drafts.set(id, { text, documentId: id, baseRevision: previous?.baseRevision ?? document.revision, gestureId, gesturePhase,
      transactionId: `edit-${Date.now()}-${++this.sequence}`, conflict: previous?.conflict });
    this.changed();
  }
  private enqueue<T>(work: () => Promise<T>): Promise<T> {
    ++this.pendingOperations;
    const job = this.tail.then(work).finally(() => { --this.pendingOperations; this.changed(); });
    this.tail = job.catch(() => {}); return job;
  }
  private async flushActive() {
    let draft: Draft | undefined;
    while ((draft = this.drafts.get(this.snapshot?.document.id ?? ''))) {
      if (draft.conflict) throw new Error('The native source changed while you were editing. Your local draft is preserved.');
      const sent = draft; this.applying = sent;
      try {
        const result = await this.transport('document.apply', { ...sent });
        const applied = (result?.snapshot ?? result)?.document as Document | undefined;
        if (!applied || applied.id !== sent.documentId) throw new Error('The instrument changed before its source was acknowledged. Your draft is preserved.');
        const newer = this.drafts.get(sent.documentId);
        if (newer === sent) this.drafts.delete(sent.documentId);
        else if (newer) newer.baseRevision = applied.revision;
        this.receive(result);
      } catch (error) {
        const current = this.drafts.get(sent.documentId) ?? sent; current.conflict = true; this.drafts.set(sent.documentId, current);
        this.changed(); throw error;
      } finally { this.applying = undefined; }
    }
  }
  private async switchTo(id: string) {
    if (!id || this.snapshot?.document.id === id) return;
    await this.flushActive();
    this.receive(await this.transport('document.activate', { documentId: id }));
    if (this.snapshot?.document.id !== id) throw new Error('The requested instrument could not be activated.');
    // Activation advances the session revision without changing the source.
    const draft = this.drafts.get(id), document = this.document(id);
    if (draft && document && !draft.conflict) draft.baseRevision = document.revision;
  }
  activate(id: string) {
    return this.enqueue(async () => { await this.switchTo(id); await this.flushActive(); });
  }
  flush() { return this.enqueue(() => this.flushActive()); }
  execute(method: string, params: Record<string, unknown> = {}, id?: string) {
    return this.enqueue(async () => {
      if (id) await this.switchTo(id);
      await this.flushActive();
      const document = this.snapshot?.document;
      const result = await this.transport(method, { ...(method.startsWith('document.') || method === 'compile' ? { documentId: document?.id } : {}),
        ...(method === 'compile' ? { revision: document?.revision } : {}), ...params });
      this.receive(result); return result;
    });
  }
  close(id: string, choice?: 'save' | 'discard') {
    return this.enqueue(async () => {
      const previous = this.snapshot?.document.id;
      // Native close/save can target an inactive document. Only an unsent local
      // source needs activation, because apply is revision-scoped to the engine.
      if (choice !== 'discard' && this.drafts.has(id)) { await this.switchTo(id); await this.flushActive(); }
      const result = await this.transport('document.close', { documentId: id, ...(choice ? { [choice]: true } : {}) });
      // Keep a discarded local draft recoverable until the close succeeds.
      this.drafts.delete(id); this.receive(result);
      if (previous && previous !== id && this.snapshot?.document.id !== previous) await this.switchTo(previous);
      return result;
    });
  }
  reload(id: string) { this.drafts.delete(id); this.changed(); }
  applyDraft(id: string) {
    const draft = this.drafts.get(id), document = this.document(id);
    if (draft && document) { draft.baseRevision = document.revision; draft.conflict = false; this.changed(); }
    return this.activate(id);
  }
}

export type EditorGroup = { tabs: string[]; active: string };
export function openTab(groups: EditorGroup[], index: number, id: string): EditorGroup[] {
  return groups.map((group, i) => i !== index ? group : { tabs: group.tabs.includes(id) ? group.tabs : [...group.tabs, id], active: id });
}
export function closeTab(groups: EditorGroup[], index: number, id: string): EditorGroup[] {
  return groups.map((group, i) => {
    if (i !== index) return group;
    const position = group.tabs.indexOf(id), tabs = group.tabs.filter(tab => tab !== id);
    return { tabs, active: group.active === id ? tabs[Math.min(position, tabs.length - 1)] ?? '' : group.active };
  });
}
export function reconcileGroups(groups: EditorGroup[], ids: string[], active: string): EditorGroup[] {
  const result = groups.map(group => {
    const tabs = group.tabs.filter(id => ids.includes(id));
    return { tabs, active: tabs.includes(group.active) ? group.active : tabs[0] ?? '' };
  });
  for (const id of ids) if (!result.some(group => group.tabs.includes(id))) result[0].tabs.push(id);
  if (active && !result.some(group => group.active === active)) result[0].active = active;
  if (!result[0].active) result[0].active = result[0].tabs[0] ?? '';
  return result;
}
