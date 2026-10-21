/** A predefined instruction document owned by the Hub operator. */
export interface SkillSummary {
    /** Filename stem, used by @{skill:ID}; never a path. */
    id: string;
    /** Stable author-provided name. */
    name: string;
    /** Short display title. */
    title: string;
    /** Selection guidance, not injected into the message. */
    description: string;
    /** Optional search terms, normalized to an empty list. */
    keywords: string[];
    /** Content hash identifying the exact file snapshot. */
    revision: string;
}
/** Validated instruction snapshot. Only text is expanded into user input. */
export interface SkillDocument extends SkillSummary {
    /** Literal UTF-8 instructions, inserted without recursive expansion. */
    text: string;
}
/** Invalid files are reported individually without hiding valid skills. */
export interface SkillList {
    skills: SkillSummary[];
    errors: { id: string; message: string }[];
}
/** YAML source for authoring a new file; reading it never creates a file. */
export interface SkillTemplate { text: string; }
