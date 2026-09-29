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
