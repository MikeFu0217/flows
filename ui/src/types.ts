export type Projection = 'mid' | 'left' | 'right' | 'side';
export interface Document { id: string; path: string; text: string; revision: number; savedRevision?: number; dirty?: boolean }
export interface Signal { name: string; kind?: string; shape?: string; unit?: string; captureRoots?: number[]; channels?: number; auditionable?: boolean; from: number; to: number }
export interface Parameter { id: string; name?: string; from: number; to: number; value: number; unit?: string; hasRange?: boolean; min: number; max: number; step?: number; discrete?: boolean; scale?: string; response?: string; responseOffset?: number; update?: string; hasDefault?: boolean; defaultValue?: number }
export interface ChoiceOption { value: string; enabled: boolean; reason?: string }
export interface Choice { id: string; name: string; value: string; from: number; to: number; implicit: boolean; prefix: string; suffix: string; options: ChoiceOption[] }
export interface FlowNode { id: string; label: string; op: string; kind: 'audio' | 'control' | 'analysis'; signal: string; from: number; to: number; parameterIds: string[]; choiceIds?: string[] }
export interface FlowEdge { id: string; source: string; target: string; kind: string; label?: string }
export interface FlowGraph { error?: string; nodes: FlowNode[]; edges: FlowEdge[]; output: string }
export interface Diagnostic { message: string; severity?: 'error' | 'warning' | 'info'; from?: number; to?: number; line?: number }
export interface CompileView { status: string; revision?: number; diagnostics?: Diagnostic[]; signals?: Signal[]; parameters?: Parameter[]; choices?: Choice[]; graph?: FlowGraph; voiceLimit?: number; outputChannels?: number }
export interface OpenDocument extends Document { compile: CompileView }
export interface SampleAsset { path: string; name: string; duration: number; channels: number; sampleRate: number }
export interface Snapshot {
  sessionId: string;
  workspaceId?: string;
  capabilities?: { windowTransparency?: boolean };
  document: Document;
  documents?: OpenDocument[];
  files: { path: string; name?: string; kind?: string }[];
  samples?: SampleAsset[];
  compile: CompileView;
  observation?: { voice: number; signals: string[]; projection?: Projection };
  sequencer?: SequencerState;
  audio: { bpm?: number; tempoSource?: string; running?: boolean; sampleRate?: number; blockSize?: number; voices?: number; cpu?: number; peak?: number; programRevision?: number; demoRunning?: boolean; audition?: string | null };
  ai: { profiles?: { id: string; name: string; model: string; endpoint: string; hasKey: boolean }[]; selectedId?: string; settingsError?: string; configured?: boolean; model?: string; endpoint?: string; busy?: boolean; runningConversationId?: string; runningRequestId?: string; activeConversationId?: string; historyError?: string };
  preferences?: { theme?: string; opacity?: number; split?: string; fontFamily?: string; fontSize?: number; leftWidth?: number; rightWidth?: number; flowHeight?: number; splitRatio?: number; layoutVersion?: number; showExplorer?: boolean; showAssistant?: boolean; showDataflow?: boolean; showKeyboard?: boolean };
  exportedParameters?: { slot: number; bindingId: string; name: string; bound: boolean; value?: number }[];
}
export interface TelemetrySignal { spectrumPeak?: boolean; signedSpectrum?: boolean; detail?: number[]; detailSeconds?: number; beatAvailable?: boolean; beatPhase?: number; beatMin?: number[]; beatMax?: number[]; beatAge?: number[]; held?: boolean; spectrumHeld?: boolean; waiting?: string; sampleCount?: number; windowReady?: boolean; name: string; kind?: string; captureKind?: string; shape?: string; unit?: string; values?: number[]; scalar?: number; value?: number; valid?: boolean; error?: string; rms?: number; peak?: number; spectrum?: number[]; frequencies?: number[]; spectrumFrequencies?: number[] }
export interface Telemetry { captureIdle?: boolean; captureMode?: 'key' | 'performance' | 'transport'; captureHeld?: boolean; keyEpoch?: number; sequencerStep?: number; sequencerPlaying?: boolean; sequencerDocumentId?: string; captureProjection?: Projection; frozen?: boolean; status?: string; error?: string; captureVoice?: number; generation?: number; parameterEpoch?: number; observationEpoch?: number; captureStart?: number; captureEnd?: number; captureResets?: number; analysisReady?: boolean; sequence: number; programRevision?: number; sampleRate?: number; signals: TelemetrySignal[]; outputPeak?: number; voices?: number; cpu?: number; gap?: boolean }
export interface AgentActivity { id: string; kind: 'thinking' | 'tool'; label: string; detail?: string; state: 'running' | 'done' | 'failed' | 'cancelled'; durationMs?: number }
export interface AgentEvent { activity?: AgentActivity; requestId: string; conversationId?: string; documentId?: string; status?: string; delta?: string; text?: string; proposedSource?: string; proposalAvailable?: boolean; baseRevision?: number; error?: string }
import type { SequencerState } from './sequencer';
