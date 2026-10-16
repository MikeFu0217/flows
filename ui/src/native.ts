type ListenerToken = [string, number];
interface Backend { emitEvent(name: string, payload: unknown): void; addEventListener(name: string, listener: (payload: any) => void): ListenerToken; removeEventListener(token: ListenerToken): void }
declare global { interface Window { __JUCE__?: { backend?: Backend; initialisationData?: { __juce__functions?: string[] } } } }

export const nativeAvailable = Boolean(window.__JUCE__?.backend && window.__JUCE__?.initialisationData?.__juce__functions?.includes('flowsRequest'));
let sequence = 0;
const waiting = new Map<number, { resolve: (value: any) => void; reject: (reason: Error) => void; timer: ReturnType<typeof setTimeout> }>();
if (nativeAvailable) window.__JUCE__!.backend!.addEventListener('__juce__complete', ({ promiseId, result }) => {
  const pending = waiting.get(promiseId);
  if (!pending) return;
  clearTimeout(pending.timer); waiting.delete(promiseId);
  if (result?.ok === false) pending.reject(new Error(result.error?.message || result.error || 'Native request failed'));
  else pending.resolve(result?.result ?? result);
});

export function request<T = any>(method: string, params: Record<string, unknown> = {}): Promise<T> {
  if (!nativeAvailable) return Promise.reject(new Error('Open Flows in its native host to use audio, files and AI.'));
  const resultId = ++sequence;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { waiting.delete(resultId); reject(new Error(`${method} timed out`)); }, method.startsWith('ai.') ? 180000 : 30000);
    waiting.set(resultId, { resolve, reject, timer });
    window.__JUCE__!.backend!.emitEvent('__juce__invoke', {
      name: 'flowsRequest', params: [{ id: String(resultId), method, params }], resultId,
    });
  });
}
export function subscribe<T>(event: string, callback: (value: T) => void): () => void {
  if (!nativeAvailable) return () => {};
  const backend = window.__JUCE__!.backend!;
  const token = backend.addEventListener(event, callback);
  return () => backend.removeEventListener(token);
}
