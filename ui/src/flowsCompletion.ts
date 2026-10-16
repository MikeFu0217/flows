import { CompletionContext, CompletionResult, snippetCompletion } from '@codemirror/autocomplete';
import { completeFlows } from './completionModel';

export function flowsCompletion(samples: () => readonly { path: string }[]) {
  return (context: CompletionContext): CompletionResult | null => {
    if (context.state.readOnly) return null;
    const result = completeFlows(context.state.doc.toString(), context.pos, context.explicit, samples());
    if (!result?.options.length) return null;
    return { from: result.from, to: result.to, options: result.options.map(option => {
      const info = [result.signature, option.info].filter(Boolean).join('\n\n');
      const completion = { label: option.label, type: option.type, detail: option.detail, info, boost: option.boost };
      return option.snippet ? snippetCompletion(option.snippet, completion) : { ...completion, apply: option.insert || option.label };
    }) };
  };
}
