import { useEffect, useRef, useState } from 'react';

interface Props {
  label: string; orientation: 'vertical' | 'horizontal'; controls?: string;
  value: number; min: number; max: number; defaultValue: number;
  onChange: (value: number) => void; onReset?: () => void; reverse?: boolean; pixelsPerUnit?: number;
  step?: number; unit?: string;
}

/** Layout only: dragging never writes to the instrument source. */
export function ResizeHandle({ label, orientation, controls, value, min, max, defaultValue, onChange, onReset, reverse = false, pixelsPerUnit = 1, step = 10, unit = 'px' }: Props) {
  const drag = useRef<{ pointer: number; position: number; value: number }>();
  const [active, setActive] = useState(false);
  const clamp = (next: number) => Math.max(min, Math.min(max, next));
  const finish = () => { drag.current = undefined; setActive(false); delete document.body.dataset.resizeAxis; };
  useEffect(() => () => { delete document.body.dataset.resizeAxis; }, []);
  return <div className={`resize-handle ${orientation} ${active ? 'dragging' : ''}`} role="separator" tabIndex={0}
    aria-label={label} aria-orientation={orientation} aria-controls={controls}
    aria-valuemin={Math.round(min)} aria-valuemax={Math.round(max)} aria-valuenow={Math.round(value)} aria-valuetext={`${Math.round(value)} ${unit}`}
    title={`${label} · drag or arrow keys · double-click to reset`}
    onDoubleClick={() => onReset ? onReset() : onChange(clamp(defaultValue))}
    onKeyDown={event => {
      const decrease = orientation === 'vertical' ? 'ArrowLeft' : 'ArrowUp';
      const increase = orientation === 'vertical' ? 'ArrowRight' : 'ArrowDown';
      if (![decrease, increase, 'Home', 'End'].includes(event.key)) return;
      event.preventDefault(); event.stopPropagation();
      if (event.key === 'Home') onChange(min);
      else if (event.key === 'End') onChange(max);
      else onChange(clamp(value + (event.key === increase ? 1 : -1) * (reverse ? -1 : 1) * step * (event.shiftKey ? 4 : 1)));
    }}
    onPointerDown={event => {
      if (event.button !== 0) return;
      event.preventDefault();
      event.currentTarget.setPointerCapture(event.pointerId);
      drag.current = { pointer: event.pointerId, position: orientation === 'vertical' ? event.clientX : event.clientY, value };
      document.body.dataset.resizeAxis = orientation; setActive(true);
    }}
    onPointerMove={event => {
      const start = drag.current; if (!start || start.pointer !== event.pointerId) return;
      const position = orientation === 'vertical' ? event.clientX : event.clientY;
      onChange(clamp(start.value + (position - start.position) / Math.max(.001, pixelsPerUnit) * (reverse ? -1 : 1)));
    }}
    onPointerUp={finish} onPointerCancel={finish} onLostPointerCapture={finish} />;
}
