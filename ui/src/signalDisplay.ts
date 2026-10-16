import type { Signal } from './types';
export type SignalDisplayMode = 'wave' | 'spectrum';
type Shape = Pick<Signal, 'kind' | 'shape' | 'auditionable'>;
const preferences = new Map<string, SignalDisplayMode>();
const listeners = new Set<() => void>();
export const displayKey = (documentId: string, name: string) => JSON.stringify([documentId, name]);
export const isSpectral = (signal: Shape) => signal.shape === 'spectrum' || signal.shape === 'mel_spectrum';
export const canSwitchDisplay = (signal: Shape) => signal.shape !== 'scalar' && !isSpectral(signal);
export function signalDisplay(key: string, signal: Shape): SignalDisplayMode {
  if (isSpectral(signal)) return 'spectrum';
  return preferences.get(key) ?? (signal.kind === 'audio' || signal.auditionable || signal.kind === 'analysis' && signal.shape === 'waveform' ? 'spectrum' : 'wave');
}
export function setSignalDisplay(key: string, mode: SignalDisplayMode) {
  preferences.set(key, mode); listeners.forEach(listener => listener());
}
export function onSignalDisplay(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener); }; }

let dismissMenu: (() => void) | undefined;
// Shared by the CodeMirror widget and React previews. Changing presentation does
// not request a new capture or alter the running instrument.
export function openSignalDisplayMenu(anchor: HTMLElement, key: string, signal: Shape, inspect?: () => void) {
  dismissMenu?.();
  const menu = document.createElement('div'); menu.className = 'signal-display-menu';
  menu.setAttribute('role', 'menu'); menu.setAttribute('aria-label', 'Signal display');
  const items: HTMLButtonElement[] = [];
  const close = (restore = false) => {
    menu.remove(); window.removeEventListener('pointerdown', outside, true); window.removeEventListener('keydown', keyboard, true);
    window.removeEventListener('resize', moved); window.removeEventListener('scroll', moved, true);
    if (dismissMenu === close) dismissMenu = undefined;
    if (restore && anchor.isConnected) anchor.focus({ preventScroll: true });
  };
  const add = (label: string, action: () => void, mode?: SignalDisplayMode) => {
    const button = document.createElement('button'); button.type = 'button'; button.textContent = label;
    button.setAttribute('role', mode ? 'menuitemradio' : 'menuitem');
    if (mode) { button.dataset.mode = mode; button.setAttribute('aria-checked', String(signalDisplay(key, signal) === mode)); }
    button.addEventListener('click', event => { event.stopPropagation(); close(true); action(); });
    menu.append(button); items.push(button);
  };
  if (canSwitchDisplay(signal)) {
    add('Spectrum', () => setSignalDisplay(key, 'spectrum'), 'spectrum');
    add('Waveform', () => setSignalDisplay(key, 'wave'), 'wave');
  }
  if (inspect) add('Open inspector', inspect);
  if (!items.length) return () => {};
  const outside = (event: PointerEvent) => { if (!menu.contains(event.target as Node)) close(); };
  const moved = () => close();
  const keyboard = (event: KeyboardEvent) => {
    if (event.key === 'Escape' || event.key === 'Tab') { event.preventDefault(); event.stopImmediatePropagation(); close(true); }
    else if (['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) {
      event.preventDefault(); event.stopImmediatePropagation();
      const current = items.indexOf(document.activeElement as HTMLButtonElement);
      const index = event.key === 'Home' ? 0 : event.key === 'End' ? items.length - 1 : (current + (event.key === 'ArrowUp' ? -1 : 1) + items.length) % items.length;
      items[index].focus();
    }
  };
  document.body.append(menu);
  const rect = anchor.getBoundingClientRect(), box = menu.getBoundingClientRect();
  menu.style.left = `${Math.max(8, Math.min(innerWidth - box.width - 8, rect.left))}px`;
  menu.style.top = `${Math.max(8, Math.min(innerHeight - box.height - 8, rect.bottom + 5))}px`;
  dismissMenu = close;
  window.addEventListener('pointerdown', outside, true); window.addEventListener('keydown', keyboard, true);
  window.addEventListener('resize', moved); window.addEventListener('scroll', moved, true);
  items.find(item => item.getAttribute('aria-checked') === 'true')?.focus({ preventScroll: true });
  if (!menu.contains(document.activeElement)) items[0].focus({ preventScroll: true });
  return () => close();
}
