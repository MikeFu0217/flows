import type { Parameter } from './types';

// Defaults are semantic values, already converted by the compiler to the
// authored unit. Drag steps and display precision must not change their sound.
export function parameterDefaultLiteral(parameter: Parameter): string | null {
  return parameter.hasDefault && Number.isFinite(parameter.defaultValue)
    ? String(parameter.defaultValue)
    : null;
}
