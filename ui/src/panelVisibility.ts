export type PanelName = 'explorer' | 'assistant' | 'dataflow' | 'keyboard';

type ShortcutEvent = Pick<KeyboardEvent, 'code' | 'key' | 'metaKey' | 'ctrlKey' | 'altKey' | 'shiftKey' | 'isComposing' | 'keyCode' | 'getModifierState'>;

/** Match physical letter keys: Option+B produces “∫” on a macOS keyboard. */
export function panelShortcut(event: ShortcutEvent, mac: boolean): PanelName | null {
  if (event.isComposing || event.keyCode === 229 || event.getModifierState('AltGraph')) return null;
  if (mac ? !event.metaKey || event.ctrlKey : !event.ctrlKey || event.metaKey) return null;
  const code = event.code || `Key${event.key.toUpperCase()}`;
  if (code === 'KeyB' && !event.shiftKey) return event.altKey ? 'assistant' : 'explorer';
  if (code === 'KeyJ' && !event.altKey) return event.shiftKey ? 'keyboard' : 'dataflow';
  return null;
}

export function panelShortcutLabel(panel: PanelName, mac: boolean): string {
  const key = panel === 'explorer' || panel === 'assistant' ? 'B' : 'J';
  return mac ? `${panel === 'assistant' ? '⌥ ' : panel === 'keyboard' ? '⇧ ' : ''}⌘ ${key}`
    : `Ctrl+${panel === 'assistant' ? 'Alt+' : panel === 'keyboard' ? 'Shift+' : ''}${key}`;
}

/** Hidden panels cost no width, without changing the remembered dimensions. */
export function fitSidebars(width: number, left: number, right: number, showLeft: boolean, showRight: boolean) {
  const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));
  const centerMin = Math.min(420, Math.max(280, width * .4));
  const budget = Math.max(0, width - (Number(showLeft) + Number(showRight)) * 6 - centerMin);
  const minLeft = showLeft ? Math.min(160, budget * (showRight ? .4 : 1)) : 0;
  const minRight = showRight ? Math.min(220, budget * (showLeft ? .6 : 1)) : 0;
  const extraLeft = showLeft ? Math.max(0, clamp(left, minLeft, 520) - minLeft) : 0;
  const extraRight = showRight ? Math.max(0, clamp(right, minRight, 600) - minRight) : 0;
  const scale = Math.min(1, Math.max(0, budget - minLeft - minRight) / Math.max(1, extraLeft + extraRight));
  return { left: minLeft + extraLeft * scale, right: minRight + extraRight * scale, minLeft, minRight, budget };
}
