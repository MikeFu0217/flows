import type { Choice } from './types';

// Choices are source edits described by the compiler, never inferred from text
// in the UI. Callers also require the matching document revision.
export function applyChoice(source: string, choice: Choice, value: string): string | null {
  if (value === choice.value || !choice.options.some(option => option.value === value && option.enabled)) return null;
  if (!Number.isInteger(choice.from) || !Number.isInteger(choice.to) || choice.from < 0 || choice.to < choice.from || choice.to > source.length) return null;
  if (choice.implicit ? choice.from !== choice.to : source.slice(choice.from, choice.to) !== choice.value) return null;
  return source.slice(0, choice.from) + choice.prefix + value + choice.suffix + source.slice(choice.to);
}
