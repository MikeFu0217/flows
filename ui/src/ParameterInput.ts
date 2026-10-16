import type { Parameter } from './types';
import { parameterInput } from './parameterDrag';

// One precise-value editor for both the source and Signal Flow. Source ownership
// is checked by the caller before committing; outside clicks cancel the draft.
export function openParameterInput(anchor: HTMLElement, parameter: Parameter, suffix: string, commit: (literal: string) => boolean): () => void {
  const form = document.createElement('form'); form.className = 'parameter-input-popup';
  form.setAttribute('role', 'dialog'); form.setAttribute('aria-label', `Set ${parameter.name || parameter.id}`);
  const label = document.createElement('label'); label.textContent = parameter.name || 'Value';
  const input = document.createElement('input'); input.type = 'text'; input.value = `${parameter.value}${suffix}`;
  input.setAttribute('aria-label', `Value for ${parameter.name || parameter.id}`); input.autocomplete = 'off'; input.spellcheck = false;
  label.append(input);
  const hint = document.createElement('small'); hint.textContent = `${parameter.hasRange ? `${parameter.min}–${parameter.max}${suffix} · ` : ''}Enter to apply · Esc to cancel`;
  const error = document.createElement('div'); error.className = 'parameter-input-error'; error.setAttribute('role', 'alert');
  const apply = document.createElement('button'); apply.type = 'submit'; apply.textContent = 'Apply';
  form.append(label, hint, error, apply);
  const returnFocus = anchor.closest<HTMLElement>('.cm-content') ?? anchor;
  const rect = anchor.getBoundingClientRect();
  form.style.left = `${Math.max(8, Math.min(innerWidth - 258, rect.left))}px`;
  form.style.top = `${Math.max(8, Math.min(innerHeight - 180, rect.bottom + 6))}px`;
  document.body.append(form);
  let closed = false;
  const close = () => {
    if (closed) return; closed = true;
    window.removeEventListener('pointerdown', outside, true); window.removeEventListener('blur', close);
    form.remove();
    if (returnFocus.isConnected) returnFocus.focus({ preventScroll: true });
  };
  const outside = (event: PointerEvent) => { if (!form.contains(event.target as Node)) close(); };
  form.addEventListener('keydown', event => {
    event.stopPropagation();
    if (event.key === 'Escape') { event.preventDefault(); close(); }
  });
  form.addEventListener('submit', event => {
    event.preventDefault();
    const result = parameterInput(input.value, parameter, suffix);
    if ('error' in result) { error.textContent = result.error; input.setAttribute('aria-invalid', 'true'); return; }
    if (!commit(result.literal)) { error.textContent = 'The instrument changed. Reopen this value to edit it.'; return; }
    close();
  });
  input.addEventListener('input', () => { error.textContent = ''; input.removeAttribute('aria-invalid'); });
  window.addEventListener('pointerdown', outside, true); window.addEventListener('blur', close);
  input.focus(); input.select();
  return close;
}
