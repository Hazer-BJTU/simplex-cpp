/** Authenticated configuration-library API. IDs never contain paths or suffixes. */
export type ConfigKind = 'launch' | 'worker';

/** Exact source text and its opaque revision; submit that revision when saving. */
export interface ConfigFile {
    id: string;
    kind: ConfigKind;
    text: string;
    revision: string;
}

/** Lists contain names only, never source text, credentials or session tokens. */
export interface ConfigList {
    launch: string[];
    worker: string[];
}

/** Both references are required when creating or explicitly updating snapshots. */
export interface ConfigSelection {
    launchConfig: string;
    workerConfig: string;
}

/** Source selection affects template text only, never creates a session. */
export type ConfigTemplateSource = 'default' | 'deployment' | 'docker';
export interface ConfigTemplateQuery { source?: ConfigTemplateSource }
export interface ConfigTemplateResponse { text: string }
export interface ConfigValidateRequest { text: string }
export interface ConfigValidateResponse { valid: true }
/** null creates a new file; an opaque revision protects an existing file. */
export interface ConfigSaveRequest { text: string; revision: string | null }
export interface ConfigRemoveRequest { revision: string }
export interface ConfigRenameRequest { id: string; revision: string }
export interface ConfigRemoveResponse { removed: string }
/** Operator-owned source text may contain credentials. Do not log it. */
export interface ConfigPreviewRequest { launch: string }
/** URL strings with placeholder identity, not endpoint configuration objects. */
export interface ConfigPreviewResponse {
    endpoints: { events: string; confirm: string; tools: string };
}
