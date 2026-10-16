import { useEffect, useId, useLayoutEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import type { Choice } from './types';
import './ChoiceMenu.css';

interface PopupProps {
  choice: Choice;
  anchor: HTMLElement;
  id?: string;
  onSelect: (value: string) => void;
  onClose: (restoreFocus?: boolean) => void;
}

export function ChoicePopup({ choice, anchor, id, onSelect, onClose }: PopupProps) {
  const root = useRef<HTMLDivElement>(null);
  const callbacks = useRef({ onSelect, onClose }); callbacks.current = { onSelect, onClose };
  const selected = choice.options.findIndex(option => option.value === choice.value && option.enabled);
  const [active, setActive] = useState(selected >= 0 ? selected : choice.options.findIndex(option => option.enabled));
  // Consecutive key events may arrive before React commits a render. Enter
  // must choose the latest highlight, including immediately after typeahead.
  const activeRef = useRef(active);
  const highlight = (index: number) => { activeRef.current = index; setActive(index); };
  const [position, setPosition] = useState({ left: 0, top: 0, ready: false });
  const search = useRef({ text: '', at: 0 });
  useLayoutEffect(() => {
    if (!anchor.isConnected || !root.current) { callbacks.current.onClose(); return; }
    const box = anchor.getBoundingClientRect(), menu = root.current.getBoundingClientRect();
    const left = Math.max(8, Math.min(box.left, window.innerWidth - menu.width - 8));
    const below = box.bottom + 6;
    const top = Math.max(8, Math.min(below + menu.height <= window.innerHeight - 8 ? below : box.top - menu.height - 6, window.innerHeight - menu.height - 8));
    setPosition({ left, top, ready: true });
  }, [anchor]);
  // A visibility:hidden element cannot receive focus. Wait for the positioned
  // menu to become visible, then transfer keyboard ownership into the listbox.
  useLayoutEffect(() => { if (position.ready) root.current?.focus({ preventScroll: true }); }, [position.ready]);
  useEffect(() => {
    const outside = (event: PointerEvent) => {
      if (event.target instanceof Node && !root.current?.contains(event.target) && !anchor.contains(event.target)) callbacks.current.onClose();
    };
    const scroll = (event: Event) => { if (!(event.target instanceof Node) || !root.current?.contains(event.target)) callbacks.current.onClose(); };
    const focus = (event: FocusEvent) => {
      if (event.target instanceof Node && !root.current?.contains(event.target) && !anchor.contains(event.target)) callbacks.current.onClose();
    };
    const close = () => callbacks.current.onClose();
    const visibility = new MutationObserver(() => { if (!anchor.isConnected || anchor.closest('[hidden]')) close(); });
    visibility.observe(document.body, { subtree: true, attributes: true, attributeFilter: ['hidden'] });
    document.addEventListener('pointerdown', outside, true);
    document.addEventListener('scroll', scroll, true);
    document.addEventListener('focusin', focus, true);
    window.addEventListener('resize', close);
    window.addEventListener('blur', close);
    return () => {
      document.removeEventListener('pointerdown', outside, true);
      document.removeEventListener('scroll', scroll, true);
      document.removeEventListener('focusin', focus, true);
      visibility.disconnect();
      window.removeEventListener('resize', close);
      window.removeEventListener('blur', close);
    };
  }, [anchor]);
  useEffect(() => {
    if (!position.ready) return;
    const list = root.current?.querySelector<HTMLElement>('.choice-menu-options');
    const item = list?.querySelector<HTMLElement>(`[data-option-index="${active}"]`);
    if (!list || !item) return;
    // Keep keyboard selection visible without scrolling the editor or page.
    const viewport = list.getBoundingClientRect(), row = item.getBoundingClientRect();
    if (row.top < viewport.top) list.scrollTop += row.top - viewport.top;
    else if (row.bottom > viewport.bottom) list.scrollTop += row.bottom - viewport.bottom;
  }, [active, position.ready]);
  const choose = (value: string) => { callbacks.current.onSelect(value); callbacks.current.onClose(true); };
  const move = (direction: number) => {
    for (let step = 1; step <= choice.options.length; ++step) {
      const index = (Math.max(0, activeRef.current) + direction * step + choice.options.length) % choice.options.length;
      if (choice.options[index].enabled) { highlight(index); return; }
    }
  };
  return createPortal(<div ref={root} id={id} className="choice-menu" role="listbox" tabIndex={-1}
    data-workspace-panel={anchor.closest('[data-workspace-panel]')?.getAttribute('data-workspace-panel') || undefined}
    aria-label={`Choose ${choice.name}`} aria-activedescendant={active >= 0 ? `${id || 'flows-choice'}-${active}` : undefined}
    style={{ left: position.left, top: position.top, visibility: position.ready ? 'visible' : 'hidden' }}
    onPointerDown={event => event.stopPropagation()} onClick={event => event.stopPropagation()}
    onKeyDown={event => {
      event.stopPropagation();
      if (event.key === 'Escape') { event.preventDefault(); onClose(true); }
      else if (event.key === 'Tab') onClose(true);
      else if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { event.preventDefault(); move(event.key === 'ArrowDown' ? 1 : -1); }
      else if (event.key === 'Home' || event.key === 'End') {
        event.preventDefault();
        const enabled = choice.options.map((option, index) => option.enabled ? index : -1).filter(index => index >= 0);
        highlight((event.key === 'Home' ? enabled[0] : enabled[enabled.length - 1]) ?? -1);
      } else if (event.key === 'Enter' || event.key === ' ') {
        event.preventDefault(); const index = activeRef.current; if (index >= 0 && choice.options[index].enabled) choose(choice.options[index].value);
      } else if (event.key.length === 1 && !event.metaKey && !event.ctrlKey && !event.altKey) {
        event.preventDefault();
        const now = Date.now(); search.current = { text: (now - search.current.at < 700 ? search.current.text : '') + event.key.toLowerCase(), at: now };
        const index = choice.options.findIndex(option => option.enabled && option.value.toLowerCase().startsWith(search.current.text));
        if (index >= 0) highlight(index);
      }
    }}>
    <div className="choice-menu-heading">{choice.name}</div>
    <div className="choice-menu-options">{choice.options.map((option, index) => <div key={option.value} id={`${id || 'flows-choice'}-${index}`}
      role="option" aria-selected={option.value === choice.value} aria-disabled={!option.enabled} data-option-index={index} data-value={option.value}
      className={`choice-menu-option ${index === active ? 'focused' : ''} ${option.value === choice.value ? 'selected' : ''}`}
      onPointerMove={() => { if (option.enabled) highlight(index); }}
      onClick={() => { if (option.enabled) choose(option.value); }} title={option.reason || option.value}>
      <span className="choice-check" aria-hidden="true">{option.value === choice.value ? '✓' : ''}</span>
      <div><span>{option.value}</span>{!option.enabled && option.reason && <small>{option.reason}</small>}</div>
    </div>)}</div>
  </div>, document.body);
}

export function ChoiceSelect({ choice, disabled = false, onSelect, className = '', contextKey }: {
  choice: Choice; disabled?: boolean; onSelect: (value: string) => void; className?: string; contextKey?: string | number;
}) {
  const anchor = useRef<HTMLButtonElement>(null), id = useId();
  const [open, setOpen] = useState(false);
  const signature = JSON.stringify([choice.id, choice.from, choice.to, choice.value, choice.options]);
  useEffect(() => setOpen(false), [contextKey, disabled, signature]);
  return <div className={`choice-select ${className}`} onPointerDown={event => event.stopPropagation()} onClick={event => event.stopPropagation()}>
    <span className="choice-select-label">{choice.name}</span>
    <button ref={anchor} type="button" className="choice-select-button" disabled={disabled}
      data-choice-id={choice.id} aria-label={`Choose ${choice.name}: ${choice.value}`} aria-haspopup="listbox" aria-expanded={open} aria-controls={open ? id : undefined}
      title={`${choice.name}${choice.implicit ? ' · default' : ''} · choose value`} onClick={() => setOpen(value => !value)}
      onKeyDown={event => { if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { event.preventDefault(); event.stopPropagation(); setOpen(true); } }}>
      <span>{choice.value}</span><span className="choice-chevron" aria-hidden="true">⌄</span>
    </button>
    {open && !disabled && anchor.current && <ChoicePopup id={id} choice={choice} anchor={anchor.current} onSelect={onSelect}
      onClose={restore => { setOpen(false); if (restore) anchor.current?.focus({ preventScroll: true }); }}/>}</div>;
}
