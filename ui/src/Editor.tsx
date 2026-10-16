import { openParameterInput } from './ParameterInput';
import { captureState as observationState, heldLabel } from './observation';
import { useEffect, useId, useRef, useState } from 'react';
import { Annotation, Compartment, EditorState, Prec, StateEffect, StateField } from '@codemirror/state';
import { Decoration, DecorationSet, EditorView, highlightActiveLine, highlightActiveLineGutter, keymap, lineNumbers, ViewPlugin, ViewUpdate, WidgetType, drawSelection, dropCursor, rectangularSelection } from '@codemirror/view';
import { defaultKeymap, indentWithTab } from '@codemirror/commands';
import { bracketMatching, foldedRanges, foldGutter, foldKeymap, foldService, HighlightStyle, indentOnInput, StreamLanguage, syntaxHighlighting, unfoldEffect } from '@codemirror/language';
import { acceptCompletion, autocompletion, closeCompletion, closeBrackets, closeBracketsKeymap, completionKeymap } from '@codemirror/autocomplete';
import { searchKeymap, highlightSelectionMatches } from '@codemirror/search';
import { setDiagnostics } from '@codemirror/lint';
import { tags } from '@lezer/highlight';
import { Choice, Diagnostic, Parameter, Signal } from './types';
import { drawWave, onPlot, onTelemetry, signalData, telemetry } from './telemetry';
import { displayKey, signalDisplay, onSignalDisplay, openSignalDisplayMenu } from './signalDisplay';
import { parameterDrag } from './parameterDrag';
import { parameterDefaultLiteral } from './parameterDefault';
import { ChoicePopup } from './ChoiceMenu';
import { applyChoice } from './choice';
import { samplePathEdit } from './samplePath';
import { flowsCompletion } from './flowsCompletion';

const remote = Annotation.define<boolean>();
const gesture = Annotation.define<string>();
const refresh = StateEffect.define<null>();
const revealRange = StateEffect.define<{ from: number; to: number } | null>();
const revealDecoration = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(value, transaction) {
    if (transaction.docChanged) return Decoration.none;
    for (const effect of transaction.effects) if (effect.is(revealRange)) {
      if (!effect.value) return Decoration.none;
      const { from, to } = effect.value;
      return to > from ? Decoration.set([Decoration.mark({ class: 'source-reveal' }).range(from, to)]) : Decoration.none;
    }
    return value;
  },
  provide: field => EditorView.decorations.from(field),
});
const language = StreamLanguage.define({ token(stream) {
  if (stream.eatSpace()) return null;
  if (stream.match(/#.*/)) return 'comment';
  if (stream.match(/"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'/)) return 'string';
  if (stream.match(/\b(?:import|from|as|return|class|def|if|else|and|or|not)\b/)) return 'keyword';
  if (stream.match(/\b(?:Inst|std|Classic\w+|self)\b/)) return 'typeName';
  if (stream.match(/(?:\d+(?:\.\d*)?|\.\d+)(?:khz|hz|ms|db|s)?\b/i)) return 'number';
  if (stream.match(/\b(?:sigstream|poly|mono|make|probe|power|spec|spectrum|mel_spec|stereo|left|right|mid|side|register_knob)\b/)) return 'function(variableName)';
  if (stream.match(/\b(?:pitch|gate|velocity|trigger|cutoff|resonance|level|unison|file|root|rate|start|end|mode|reverse|A|D|S|R)\b/)) return 'propertyName';
  if (stream.match(/[+*\/=<>!:\-]/)) return 'operator';
  if (stream.match(/[a-zA-Z_]\w*/)) return 'variableName';
  stream.next(); return null;
} });
const highlighting = HighlightStyle.define([
  { tag: tags.keyword, color: 'var(--syntax-keyword)' }, { tag: tags.typeName, color: 'var(--syntax-type)' },
  { tag: tags.number, color: 'var(--syntax-number)' }, { tag: tags.string, color: 'var(--syntax-string)' },
  { tag: tags.comment, color: 'var(--syntax-comment)', fontStyle: 'italic' },
  { tag: tags.function(tags.variableName), color: 'var(--syntax-function)' },
  { tag: tags.propertyName, color: 'var(--syntax-property)' }, { tag: tags.operator, color: 'var(--syntax-operator)' },
]);

interface Props { text: string; documentId: string; signals: Signal[]; parameters: Parameter[]; choices: Choice[]; diagnostics: Diagnostic[]; samples?: readonly { path: string }[]; connected: boolean; perform?: boolean; metadataPending: boolean; live: boolean; visible: boolean;
  reveal?: { from: number; to: number; nonce: number };
  onChange: (text: string, gestureId?: string, phase?: string) => void; onInspect: (name: string) => void; onExport: (parameter: Parameter) => void;
  onUndo: () => void; onRedo: () => void; onSave: () => void; onCompile: () => void; }

class Capsule extends WidgetType {
  private unsubscribe?: () => void;
  private closeMenu?: () => void;
  constructor(readonly documentId: string, readonly signal: Signal, readonly inspect: (name: string) => void, readonly live: boolean, readonly isLive: () => boolean) { super(); }
  eq(other: Capsule) { return other.documentId === this.documentId && other.live === this.live && other.signal.name === this.signal.name && other.signal.kind === this.signal.kind && other.signal.shape === this.signal.shape && other.signal.unit === this.signal.unit && other.signal.auditionable === this.signal.auditionable; }
  toDOM() {
    const button = document.createElement('button'); button.className = `signal-capsule ${this.signal.kind || 'audio'}`;
    button.type = 'button'; button.draggable = true;
    button.title = `Inspect ${this.signal.name} · drag to signal flow`;
    button.setAttribute('aria-label', `Inspect signal ${this.signal.name}`);
    const dot = document.createElement('i'); const label = document.createElement('span'); label.textContent = this.signal.name;
    const canvas = document.createElement('canvas'); canvas.width = 76; canvas.height = 24;
    const scalar = document.createElement('b'); scalar.className = 'capsule-value'; scalar.hidden = true;
    const status = document.createElement('small'); status.className = 'capsule-capture-status'; status.hidden = true;
    button.append(dot, label, canvas, scalar, status);
    const key = displayKey(this.documentId, this.signal.name);
    button.addEventListener('click', event => {
      if (event.target === canvas && this.isLive()) {
        event.stopPropagation(); this.closeMenu = openSignalDisplayMenu(button, key, this.signal, () => this.inspect(this.signal.name));
      } else this.inspect(this.signal.name);
    });
    button.addEventListener('dragstart', event => { event.dataTransfer?.setData('application/flows-signal', this.signal.name); if (event.dataTransfer) event.dataTransfer.effectAllowed = 'copy'; });
    const draw = () => {
      if (!this.isLive()) {
        button.dataset.captureState = 'inactive'; button.title = `${this.signal.name} · Focus this instrument to observe its signals`;
        canvas.style.display = 'none'; scalar.hidden = true; status.hidden = true; return;
      }
      const sample = signalData(this.signal.name), isScalar = (sample?.shape ?? this.signal.shape) === 'scalar';
      const mode = signalDisplay(key, this.signal); button.dataset.viewMode = mode;
      const captureState = observationState(telemetry(), sample, mode === 'spectrum');
      status.hidden = captureState !== 'held'; status.textContent = heldLabel(telemetry());
      button.dataset.captureState = captureState;
      button.title = `Inspect ${this.signal.name} · click preview to choose Spectrum / Waveform · drag to signal flow${captureState === 'held' ? ` · ${heldLabel(telemetry())}: stored capture, not a live signal` : captureState === 'idle' ? ' · Play a key or sequencer to capture' : captureState === 'waiting' ? ' · Waiting for capture' : ''}`;
      canvas.style.display = isScalar || sample?.error ? 'none' : '';
      scalar.hidden = !isScalar && !sample?.error;
      const value = sample?.scalar ?? sample?.value;
      scalar.textContent = sample?.error ? 'error' : sample?.valid !== false && value !== undefined && Number.isFinite(value) ? Number(value.toPrecision(4)).toString() : '—';
      scalar.title = sample?.error || sample?.unit || '';
      drawWave(canvas, this.signal.name, this.signal.kind === 'control' ? '#e3bf83' : '#a8e4c2', mode === 'spectrum');
    };
    const offTelemetry = onPlot(draw), offDisplay = onSignalDisplay(draw);
    this.unsubscribe = () => { offTelemetry(); offDisplay(); }; draw(); return button;
  }
  destroy() { this.unsubscribe?.(); this.closeMenu?.(); }
  ignoreEvent() { return true; }
}

export function Editor(props: Props) {
  const container = useRef<HTMLDivElement>(null), viewRef = useRef<EditorView>();
  const menuId = useId();
  const [choiceMenu, setChoiceMenu] = useState<{ id: string; anchor: HTMLElement; source: string; documentId: string } | null>(null);
  const lastTap = useRef<{ at: number; documentId: string; parameterId: string; from: number; to: number }>();
  const latest = useRef(props); latest.current = props;
  const editable = useRef(new Compartment());
  useEffect(() => {
    if (!container.current) return;
    lastTap.current = undefined;
    const typography = new Compartment();
    const decorations = ViewPlugin.fromClass(class {
      decorations: DecorationSet;
      constructor(readonly view: EditorView) { this.decorations = this.build(); }
      update(update: ViewUpdate) {
        const value = latest.current;
        if (value.metadataPending || this.view.state.doc.toString() !== value.text) {
          // Stale widgets can land in the middle of newly typed text. Only the
          // acknowledged source may introduce inline layout or source bindings.
          this.decorations = Decoration.none;
        } else if (update.docChanged || update.viewportChanged || update.transactions.some(t => t.effects.some(e => e.is(refresh)))) this.decorations = this.build();
      }
      build() {
        const value = latest.current;
        // Source spans belong to the acknowledged source, never apply stale numeric bindings.
        if (value.metadataPending || this.view.state.doc.toString() !== value.text) return Decoration.none;
        const ranges = [];
        for (const signal of value.signals) {
          if (!Number.isInteger(signal.from) || signal.from < 0 || signal.from > this.view.state.doc.length) continue;
          const line = this.view.state.doc.lineAt(Math.min(signal.to ?? signal.from, this.view.state.doc.length));
          ranges.push(Decoration.widget({ widget: new Capsule(value.documentId, signal, name => latest.current.onInspect(name), value.live, () => latest.current.live && latest.current.visible), side: 1 }).range(line.to));
          if (signal.to > signal.from && signal.to <= this.view.state.doc.length) ranges.push(Decoration.mark({ class: `semantic-${signal.kind || 'audio'}` }).range(signal.from, signal.to));
        }
        for (const parameter of value.parameters) {
          if (parameter.from < 0 || parameter.to <= parameter.from || parameter.to > this.view.state.doc.length) continue;
          if (parameter.hasRange && parameter.max <= parameter.min) continue;
          const suffix = this.view.state.doc.sliceString(parameter.from, parameter.to).match(/[a-zA-Z]+$/)?.[0] || '';
          let title = `${parameter.hasRange ? `${parameter.min} – ${parameter.max}${suffix}` : 'Relative adjustment'} · drag to adjust · ⇧ fine · Alt+click to type`;
          if (parameter.hasDefault) title += ' · double-click for default';
          ranges.push(Decoration.mark({ class: 'draggable-number', attributes: { title, 'data-parameter-id': parameter.id } }).range(parameter.from, parameter.to));
        }
        // Composition instances may share the same authored definition. One
        // decoration per span is enough; each graph instance retains its owner.
        const spans = new Set<string>();
        for (const choice of value.choices) {
          if (choice.implicit || choice.from < 0 || choice.to <= choice.from || choice.to > this.view.state.doc.length || this.view.state.doc.sliceString(choice.from, choice.to) !== choice.value) continue;
          const span = `${choice.from}:${choice.to}`; if (spans.has(span)) continue; spans.add(span);
          ranges.push(Decoration.mark({ class: 'selectable-option', attributes: { 'data-choice-id': choice.id, role: 'button', 'aria-haspopup': 'listbox', 'aria-label': `Choose ${choice.name}: ${choice.value}`, title: `${choice.name} · click to choose · Alt+Enter at cursor · Alt+click to edit text` } }).range(choice.from, choice.to));
        }
        return Decoration.set(ranges, true);
      }
    }, { decorations: value => value.decorations });
    const customKeys = keymap.of([
      { key: 'Alt-Enter', run: current => {
        if (latest.current.metadataPending || !latest.current.connected) return false;
        const position = current.state.selection.main.head;
        const choice = latest.current.choices.find(item => !item.implicit && item.from <= position && position <= item.to);
        const anchor = choice && [...current.dom.querySelectorAll<HTMLElement>('.selectable-option')].find(item => item.dataset.choiceId === choice.id);
        if (!choice || !anchor) return false;
        setChoiceMenu({ id: choice.id, anchor, source: current.state.doc.toString(), documentId: latest.current.documentId }); return true;
      } },
      { key: 'Mod-z', run: () => { if (!latest.current.perform) latest.current.onUndo(); return true; } },
      { key: 'Mod-Shift-z', run: () => { if (!latest.current.perform) latest.current.onRedo(); return true; } },
      { key: 'Mod-y', run: () => { if (!latest.current.perform) latest.current.onRedo(); return true; } },
      { key: 'Mod-s', run: () => { latest.current.onSave(); return true; } },
      { key: 'Mod-Enter', run: () => { latest.current.onCompile(); return true; } },
    ]);
    let cleanupDrag: (() => void) | undefined;
    let cleanupInput: (() => void) | undefined;
    const view = new EditorView({ parent: container.current,
      state: EditorState.create({ doc: latest.current.text, extensions: [
        EditorState.transactionFilter.of(transaction => latest.current.perform && transaction.docChanged && !transaction.annotation(remote) && !transaction.annotation(gesture) ? [] : transaction),
        typography.of(EditorView.theme({})),
        lineNumbers(), highlightActiveLineGutter(), highlightActiveLine(), drawSelection(), dropCursor(), rectangularSelection(), revealDecoration,
        foldGutter(), foldService.of((state, start) => {
          const line = state.doc.lineAt(start); if (!line.text.trimEnd().endsWith(':')) return null;
          const indentation = line.text.length - line.text.trimStart().length; let end = line.to;
          for (let number = line.number + 1; number <= state.doc.lines; ++number) {
            const next = state.doc.line(number); if (!next.text.trim()) continue;
            if (next.text.length - next.text.trimStart().length <= indentation) break;
            end = next.to;
          }
          return end > line.to ? { from: line.to, to: end } : null;
        }), indentOnInput(), bracketMatching(), closeBrackets(), highlightSelectionMatches(), language, syntaxHighlighting(highlighting),
        autocompletion({ override: [flowsCompletion(() => latest.current.samples || [])], activateOnTyping: true, defaultKeymap: false, icons: true, maxRenderedOptions: 60 }),
        EditorView.contentAttributes.of({ tabindex: "0" }),
        EditorState.tabSize.of(4), editable.current.of([EditorView.editable.of(latest.current.connected && !latest.current.perform), EditorState.readOnly.of(!latest.current.connected || !!latest.current.perform)]),
        Prec.high(customKeys), keymap.of([{ key: 'Tab', run: acceptCompletion }, ...completionKeymap, ...closeBracketsKeymap, ...defaultKeymap, ...searchKeymap, ...foldKeymap, indentWithTab]),
        decorations,
        EditorView.updateListener.of(update => {
          if (update.docChanged) lastTap.current = undefined;
          if (update.docChanged && !update.transactions.some(t => t.annotation(remote)) && !update.view.composing)
            latest.current.onChange(update.state.doc.toString(), update.transactions.find(t => t.annotation(gesture))?.annotation(gesture));
        }),
        EditorView.domEventHandlers({
          dragover: event => {
            if (!latest.current.connected || !event.dataTransfer?.types.includes('application/flows-sample')) return false;
            event.preventDefault(); event.dataTransfer.dropEffect = 'copy'; return true;
          },
          drop: (event, current) => {
            const path = event.dataTransfer?.getData('application/flows-sample');
            if (!path || !latest.current.connected) return false;
            if (latest.current.perform) { event.preventDefault(); return true; }
            event.preventDefault();
            const position = current.posAtCoords({ x: event.clientX, y: event.clientY });
            if (position === null) return true;
            const edit = samplePathEdit(current.state.doc.toString(), position, path);
            current.dispatch({ changes: edit, selection: { anchor: edit.from + edit.insert.length }, annotations: gesture.of(`sample-${Date.now()}`) });
            current.focus(); return true;
          },
          contextmenu: (event, current) => {
            if (latest.current.metadataPending || current.state.doc.toString() !== latest.current.text) return false;
            const position = current.posAtCoords({ x: event.clientX, y: event.clientY });
            const parameter = latest.current.parameters.find(p => position !== null && p.from <= position && position < p.to);
            if (!parameter) return false;
            event.preventDefault(); latest.current.onExport(parameter); return true;
          },
          compositionend: (_event, current) => { queueMicrotask(() => { if (!latest.current.perform) latest.current.onChange(current.state.doc.toString()); }); return false; },
          pointerdown: (event, current) => {
            const previousTap = lastTap.current;
            lastTap.current = undefined;
            const option = (event.target as HTMLElement).closest<HTMLElement>('.selectable-option');
            if (option && event.button === 0 && !event.altKey && !event.metaKey && !event.ctrlKey && !event.shiftKey && latest.current.connected && !latest.current.metadataPending && current.state.doc.toString() === latest.current.text) {
              const choice = latest.current.choices.find(item => item.id === option.dataset.choiceId);
              if (choice && !choice.implicit) {
                event.preventDefault(); cleanupDrag?.();
                setChoiceMenu({ id: choice.id, anchor: option, source: current.state.doc.toString(), documentId: latest.current.documentId }); return true;
              }
            }
            if (!latest.current.connected || latest.current.metadataPending || current.state.doc.toString() !== latest.current.text || event.button !== 0 || !(event.target as HTMLElement).closest('.draggable-number')) return false;
            const number = (event.target as HTMLElement).closest<HTMLElement>('.draggable-number');
            const parameter = latest.current.parameters.find(p => p.id === number?.dataset.parameterId);
            if (!parameter || !Number.isFinite(parameter.value) || parameter.hasRange && (!Number.isFinite(parameter.min) || !Number.isFinite(parameter.max) || parameter.max <= parameter.min)) return false;
            if (event.altKey) {
              event.preventDefault(); cleanupDrag?.(); cleanupInput?.();
              const original = current.state.doc.toString(), originalId = latest.current.documentId;
              const suffix = original.slice(parameter.from, parameter.to).match(/[a-zA-Z]+$/)?.[0] || '';
              cleanupInput = openParameterInput((event.target as HTMLElement).closest<HTMLElement>('.draggable-number')!, parameter, suffix, literal => {
                if (!latest.current.connected || latest.current.metadataPending || latest.current.documentId !== originalId || current.state.doc.toString() !== original) return false;
                const id = `number-input-${Date.now()}-${parameter.id}`;
                current.dispatch({ changes: { from: parameter.from, to: parameter.to, insert: literal }, annotations: gesture.of(id) });
                latest.current.onChange(current.state.doc.toString(), id, 'commit');
                return true;
              });
              return true;
            }
            // Cancelling pointerdown suppresses the browser's own dblclick, so a
            // second press inside the double-click window is detected here.
            const doubleTap = previousTap && Date.now() - previousTap.at < 400
              && previousTap.documentId === latest.current.documentId && previousTap.parameterId === parameter.id
              && previousTap.from === parameter.from && previousTap.to === parameter.to;
            if (doubleTap) {
              const reset = parameterDefaultLiteral(parameter);
              const literal = current.state.doc.sliceString(parameter.from, parameter.to);
              const suffix = literal.match(/[a-zA-Z]+$/)?.[0] || '';
              if (reset !== null && `${reset}${suffix}` !== literal) {
                event.preventDefault();
                current.dispatch({ changes: { from: parameter.from, to: parameter.to, insert: `${reset}${suffix}` }, annotations: gesture.of(`reset-${Date.now()}`) });
                return true;
              }
            }
            event.preventDefault(); cleanupDrag?.();
            const original = current.state.doc.toString(), startX = event.clientX, originalDocumentId = latest.current.documentId;
            const gestureId = `drag-${Date.now()}-${parameter.id}`;
            const suffix = original.slice(parameter.from, parameter.to).match(/[a-zA-Z]+$/)?.[0] || '';
            const popup = document.createElement('div'); popup.className = 'range-popup';
            popup.style.left = `${Math.max(8, Math.min(innerWidth - 254, event.clientX - 120))}px`;
            popup.style.top = `${Math.max(8, event.clientY - 74)}px`; document.body.append(popup);
            let previousText = original, currentEnd = parameter.to, previousX = startX, moved = false, finished = false;
            const adjust = parameterDrag(parameter), pointerId = event.pointerId;
            const relativeStep = Math.max(Math.abs(parameter.value), 1) * .01;
            const initialSelection = current.state.selection;
            const show = (value: number) => { popup.replaceChildren(); const label = document.createElement('strong'); label.textContent = `${value}${suffix}`; const range = document.createElement('span'); range.textContent = `${parameter.hasRange ? `${parameter.min}${suffix}  —  ${parameter.max}${suffix}` : `relative · ${Number(relativeStep.toPrecision(5))}${suffix}/px`} · ${parameter.update || 'live'} · ⇧ fine`; popup.append(label, range); };
            show(parameter.value); document.body.classList.add('number-dragging');
            const move = (moveEvent: PointerEvent) => {
              if (moveEvent.pointerId !== pointerId) return;
              if (latest.current.documentId !== originalDocumentId || current.state.doc.toString() !== previousText) { finish(); return; }
              const delta = moveEvent.clientX - previousX; previousX = moveEvent.clientX;
              if (!delta) return;
              moved = true;
              const value = adjust(delta, moveEvent.shiftKey); show(value);
              const text = original.slice(0, parameter.from) + value + suffix + original.slice(parameter.to);
              if (text !== previousText) {
                previousText = text;
                const literal = `${value}${suffix}`;
                current.dispatch({ changes: { from: parameter.from, to: currentEnd, insert: literal }, annotations: gesture.of(gestureId) });
                currentEnd = parameter.from + literal.length;
              }
            };
            const finish = (cancel = false) => {
              if (finished) return; finished = true;
              window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); window.removeEventListener('pointercancel', cancelPointer); window.removeEventListener('keydown', escape, true); window.removeEventListener('blur', blur);
              popup.remove(); document.body.classList.remove('number-dragging'); cleanupDrag = undefined;
              if (!cancel && !moved && latest.current.documentId === originalDocumentId)
                lastTap.current = { at: Date.now(), documentId: originalDocumentId, parameterId: parameter.id, from: parameter.from, to: parameter.to };
              if (latest.current.documentId === originalDocumentId && current.state.doc.toString() === previousText) {
                if (cancel) current.dispatch({ changes: { from: parameter.from, to: currentEnd, insert: original.slice(parameter.from, parameter.to) }, selection: initialSelection, annotations: remote.of(true) });
                latest.current.onChange(cancel ? original : current.state.doc.toString(), gestureId, cancel ? 'cancel' : 'commit');
              }
            };
            const up = (event: PointerEvent) => { if (event.pointerId === pointerId) finish(); }, cancelPointer = (event: PointerEvent) => { if (event.pointerId === pointerId) finish(true); };
            const blur = () => finish();
            const escape = (key: KeyboardEvent) => { if (key.key === 'Escape') { key.preventDefault(); key.stopPropagation(); finish(true); } };
            cleanupDrag = () => finish(true);
            window.addEventListener('pointermove', move); window.addEventListener('pointerup', up); window.addEventListener('pointercancel', cancelPointer); window.addEventListener('keydown', escape, true); window.addEventListener('blur', blur);
            return true;
          },
        }),
      ] }),
    }); viewRef.current = view;
    // Root CSS font changes can leave a short document's content box the same
    // height. requestMeasure alone then skips the cached line metrics, leaving
    // gutter rows at the old pitch. A theme reconfiguration invalidates those
    // metrics through CodeMirror's public API, without rebuilding the editor.
    let measuredTypography = '';
    const measure = () => {
      const style = getComputedStyle(view.scrollDOM);
      const next = `${style.fontFamily}|${style.fontSize}|${style.lineHeight}`;
      if (next !== measuredTypography) {
        measuredTypography = next;
        view.dispatch({ effects: typography.reconfigure(EditorView.theme({})) });
        view.requestMeasure();
        // WebKit may defer animation frames for an obscured/transparent window.
        // Flush the font change now so code and gutters appear together.
        view.coordsAtPos(view.state.selection.main.head);
      }
      view.requestMeasure();
    };
    const layout = new ResizeObserver(measure); layout.observe(view.dom); layout.observe(view.contentDOM);
    const appearance = new MutationObserver(measure); appearance.observe(document.documentElement, { attributes: true, attributeFilter: ['style', 'data-theme'] });
    document.fonts?.addEventListener('loadingdone', measure);
    void document.fonts?.ready.then(() => { if (viewRef.current === view) measure(); });
    const clearTap = () => { lastTap.current = undefined; };
    const outsidePointer = (event: PointerEvent) => { if (!(event.target instanceof Node) || !view.dom.contains(event.target)) clearTap(); };
    window.addEventListener('blur', clearTap); window.addEventListener('keydown', clearTap, true); window.addEventListener('pointerdown', outsidePointer, true);
    return () => {
      window.removeEventListener('blur', clearTap); window.removeEventListener('keydown', clearTap, true); window.removeEventListener('pointerdown', outsidePointer, true);
      cleanupDrag?.(); cleanupInput?.(); clearTap(); view.destroy(); viewRef.current = undefined;
      layout.disconnect(); appearance.disconnect(); document.fonts?.removeEventListener('loadingdone', measure);
    };
  }, [props.documentId]);
  useEffect(() => {
    const view = viewRef.current; if (!view || view.composing) return;
    const previous = view.state.doc.toString();
    if (previous !== props.text) {
      let from = 0, oldEnd = previous.length, newEnd = props.text.length;
      while (from < oldEnd && from < newEnd && previous[from] === props.text[from]) ++from;
      while (oldEnd > from && newEnd > from && previous[oldEnd - 1] === props.text[newEnd - 1]) { --oldEnd; --newEnd; }
      view.dispatch({ changes: { from, to: oldEnd, insert: props.text.slice(from, newEnd) }, annotations: remote.of(true) });
    }
    view.dispatch({ effects: [refresh.of(null), editable.current.reconfigure([EditorView.editable.of(props.connected && !props.perform), EditorState.readOnly.of(!props.connected || !!props.perform)])] });
    const diagnostics = props.diagnostics.map(d => ({ from: Math.min(view.state.doc.length, Math.max(0, d.from ?? 0)), to: Math.min(view.state.doc.length, Math.max(0, d.to ?? d.from ?? 0)), severity: d.severity || 'error' as const, message: d.message }));
    view.dispatch(setDiagnostics(view.state, diagnostics));
    if (props.perform || !props.visible || !props.connected) closeCompletion(view);
    view.requestMeasure();
  }, [props.text, props.parameters, props.choices, props.signals, props.diagnostics, props.connected, props.perform, props.metadataPending, props.live, props.visible]);
  useEffect(() => { setChoiceMenu(null); }, [props.text, props.documentId, props.metadataPending, props.visible, props.connected]);
  useEffect(() => { if (props.visible) viewRef.current?.requestMeasure(); }, [props.visible]);
  useEffect(() => {
    const view = viewRef.current, reveal = props.reveal;
    if (!view || !reveal || props.metadataPending) return;
    const from = Math.max(0, Math.min(view.state.doc.length, reveal.from));
    const to = Math.max(from, Math.min(view.state.doc.length, reveal.to));
    const effects: StateEffect<unknown>[] = [];
    foldedRanges(view.state).between(from, to, (start, end) => { effects.push(unfoldEffect.of({ from: start, to: end })); });
    effects.push(revealRange.of({ from, to }), EditorView.scrollIntoView(from, { y: 'center' }));
    view.dispatch({ effects });
    const timer = setTimeout(() => { if (viewRef.current === view) view.dispatch({ effects: revealRange.of(null) }); }, 2200);
    return () => clearTimeout(timer);
  }, [props.reveal, props.documentId]);
  const selectedChoice = choiceMenu && props.choices.find(choice => choice.id === choiceMenu.id);
  return <><div className="code-editor" ref={container}/>{choiceMenu && selectedChoice && props.connected && props.visible && !props.metadataPending && choiceMenu.source === props.text && choiceMenu.documentId === props.documentId &&
    <ChoicePopup id={menuId} choice={selectedChoice} anchor={choiceMenu.anchor} onClose={restore => { setChoiceMenu(null); if (restore) viewRef.current?.focus(); }}
      onSelect={value => {
        const current = viewRef.current, state = latest.current;
        if (!current || state.documentId !== choiceMenu.documentId || state.metadataPending || current.state.doc.toString() !== choiceMenu.source || state.text !== choiceMenu.source) return;
        const choice = state.choices.find(item => item.id === choiceMenu.id);
        if (!choice || applyChoice(choiceMenu.source, choice, value) === null) return;
        current.dispatch({ changes: { from: choice.from, to: choice.to, insert: choice.prefix + value + choice.suffix }, annotations: gesture.of(`choice-${Date.now()}-${choice.id}`) });
      }}/>}</>;
}
