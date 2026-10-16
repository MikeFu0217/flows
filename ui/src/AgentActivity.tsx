import { useEffect, useState } from 'react';
import type { AssistantMessage } from './assistantHistory';
import { Icon } from './Icon';

export function AgentActivityView({ message }: { message: AssistantMessage }) {
  const working = message.status === 'working';
  const steps = message.activity || [];
  const [now, setNow] = useState(Date.now), [expanded, setExpanded] = useState(false);
  useEffect(() => {
    if (!working) return;
    setNow(Date.now());
    const timer = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, [working, message.id]);
  if (!working && !steps.length) return null;
  const active = [...steps].reverse().find(step => step.state === 'running');
  const seconds = message.startedAt ? Math.max(0, Math.floor(((message.finishedAt || now) - message.startedAt) / 1000)) : null;
  const outcome = message.status === 'done' || message.status === 'completed' ? 'Completed' : message.status === 'cancelled' || message.status === 'canceled' ? 'Stopped' : 'Ended';
  return <section className={'agent-activity ' + (working ? 'is-working' : '')} aria-label="Assistant activity">
    {working && <div className="agent-activity-current" role="status" aria-live="polite">
      <span className="agent-activity-pulse" aria-hidden="true"><Icon name="spark" size={15}/></span>
      <div><strong>{active?.label || 'Preparing request'}</strong><p>{active?.detail || 'Getting the conversation ready…'}</p></div>
    </div>}
    <button className="agent-activity-toggle" aria-expanded={expanded} onClick={() => setExpanded(!expanded)} disabled={!steps.length}>
      <Icon name={expanded ? 'down' : 'chevron'} size={12}/><span>{working ? 'Activity' : outcome}{steps.length ? ` · ${steps.length} ${steps.length === 1 ? 'step' : 'steps'}` : ''}</span>
      {seconds !== null && <time aria-label="Elapsed time">{seconds < 60 ? `${seconds}s` : `${Math.floor(seconds / 60)}m ${seconds % 60}s`}</time>}
    </button>
    {expanded && <ol className="agent-activity-steps">{steps.map(step => <li className={'activity-' + step.state} key={step.id}>
      <span className="agent-step-marker" aria-hidden="true"><Icon name={step.state === 'done' ? 'check' : step.state === 'running' ? 'circle' : 'close'} size={12}/></span>
      <div><strong>{step.label}</strong>{step.detail && <p>{step.detail}</p>}</div>
      {step.durationMs !== undefined && <time>{(step.durationMs / 1000).toFixed(1)}s</time>}
    </li>)}</ol>}
  </section>;
}
