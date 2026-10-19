/** Authenticated configuration-library API. IDs never contain paths or suffixes. */
export type ConfigKind = 'launch' | 'worker';

/** Exact source text and its opaque revision; submit that revision when saving. */
export interface ConfigFile {
    /** Configuration library name, without a path or filename suffix. */
    id: string;
    /** Configuration category: launcher JSONC or worker YAML. */
    kind: ConfigKind;
    /** Exact editable source; may contain secrets and must not be logged. */
    text: string;
    /** Opaque revision returned by the Hub; use unchanged for optimistic concurrency. */
    revision: string;
}

/** Lists contain names only, never source text, credentials or session tokens. */
export interface ConfigList {
    /** Saved launcher names only; no source text or credentials. */
    launch: string[];
    /** Saved worker configuration names only. */
    worker: string[];
}

/** Both references are required when creating or explicitly updating snapshots. */
export interface ConfigSelection {
    /** Selected launcher library name. */
    launchConfig: string;
    /** Selected worker library name. */
    workerConfig: string;
}

/** Source selection affects template text only, never creates a session. */
export type ConfigTemplateSource = 'default' | 'docker' | 'managed';
/** Template selection query; omission selects the default template. */
export interface ConfigTemplateQuery {
    /** Template source selection; omission requests the default template. */
    source?: ConfigTemplateSource;
}
/** Editable template source; reading it does not create a saved configuration. */
export interface ConfigTemplateResponse {
    /** Editable source text; may contain secrets. */
    text: string;
}
/** Source text to validate without saving or starting a worker. */
export interface ConfigValidateRequest {
    /** Editable source text; may contain secrets. */
    text: string;
}
/** Successful syntax/configuration validation, not proof that a worker can start. */
export interface ConfigValidateResponse {
    /** True after successful validation; startup may still fail on local resources. */
    valid: true;
}
/** null creates a new file; an opaque revision protects an existing file. */
export interface ConfigSaveRequest {
    /** Editable source text; may contain secrets. */
    text: string;
    /** Snapshot revision used to detect changes while reading or editing. */
    revision: string | null;
}
/** Delete a saved configuration only if its revision still matches. */
export interface ConfigRemoveRequest {
    /** Snapshot revision used to detect changes while reading or editing. */
    revision: string;
}
/** Rename to another library name using the current revision as a concurrency guard. */
export interface ConfigRenameRequest {
    /** Opaque record identifier in the enclosing contract. */
    id: string;
    /** Snapshot revision used to detect changes while reading or editing. */
    revision: string;
}
/** Name of the removed library entry; existing session snapshots are retained. */
export interface ConfigRemoveResponse {
    /** Identifier of the removed session or configuration entry. */
    removed: string;
}
/** Operator-owned source text may contain credentials. Do not log it. */
export interface ConfigPreviewRequest {
    /** Launcher source text used to preview endpoint URLs. */
    launch: string;
}
/** URL strings with placeholder identity, not endpoint configuration objects. */
export interface ConfigPreviewResponse {
    /** Preview URLs with placeholder identity; not secrets or live connection credentials. */
    endpoints: { events: string; confirm: string; tools: string };
}
