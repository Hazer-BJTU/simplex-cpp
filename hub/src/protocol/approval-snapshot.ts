import { approvalPreviewBudgets } from './approval-preview.ts';
import { PANEL_VERSION } from '../../shared/protocol.ts';
import type { ConfirmationPrompt, HubMessage, SessionDescription } from '../../shared/protocol.ts';

/** Shared argument allowance for all approvals in a single panel snapshot. */
export const APPROVAL_SNAPSHOT_ARGUMENT_BYTES = 512 * 1024;
const OMITTED = { display_omitted: true };
const OMITTED_BYTES = Buffer.byteLength(JSON.stringify(OMITTED));

/** Visit only session descriptions, leaving events and single-prompt broadcasts intact. */
function mapSessions(message: HubMessage, project: (session: SessionDescription) => SessionDescription): HubMessage {
    switch (message.type) {
        case 'welcome':
        case 'sessions':
            return { ...message, sessions: message.sessions.map(project) };
        case 'session':
        case 'created':
        case 'subscribed':
        case 'snapshot':
            return { ...message, session: project(message.session) };
        default:
            return message;
    }
}

/**
 * Share one argument budget across every session in a snapshot, and reduce it
 * further when other frame contents need space. Identity/state are never shed.
 * The regeneration callback reads the authoritative pending call so repeated
 * snapshots do not wrap an existing truncation marker in another preview.
 * Single-prompt events and REST retain the ordinary per-approval allowance.
 */
export function approvalSnapshot(
    message: HubMessage,
    frameBytes: number,
    describe: (prompt: ConfirmationPrompt, argumentBytes: number) => ConfirmationPrompt,
): HubMessage {
    const sizes: number[] = [];
    const minimal = mapSessions(message, session => ({
        ...session,
        confirmations: session.confirmations.map(prompt => {
            const args = prompt.call.arguments === undefined ? {} : prompt.call.arguments;
            const bytes = Buffer.byteLength(JSON.stringify(args));
            sizes.push(bytes);
            if (bytes <= OMITTED_BYTES) return prompt;
            return {
                ...prompt,
                arguments_truncated: true,
                arguments_bytes: prompt.arguments_bytes ?? bytes,
                call: { ...prompt.call, arguments: OMITTED },
            };
        }),
    }));
    if (sizes.length === 0) return message;

    // Include the version, all metadata, logs, replay events, and omission flags
    // before allocating arguments. Each minimal marker is replaced, not added.
    const minimalArguments = sizes.reduce((sum, size) => sum + Math.min(size, OMITTED_BYTES), 0);
    const fixedBytes = Buffer.byteLength(JSON.stringify({ v: PANEL_VERSION, ...minimal })) - minimalArguments;
    const budget = Math.min(APPROVAL_SNAPSHOT_ARGUMENT_BYTES, frameBytes - fixedBytes);
    const limits = approvalPreviewBudgets(sizes, budget);
    // If even identities/state plus minimal markers cannot fit, the existing
    // hard frame guard reports the oversized snapshot. Do not hide approvals.
    if (!limits) return minimal;

    let index = 0;
    return mapSessions(message, session => ({
        ...session,
        confirmations: session.confirmations.map(prompt => {
            const current = index++;
            if (limits[current]! >= sizes[current]!) return prompt;
            const preview = describe(prompt, limits[current]!);
            return {
                ...prompt,
                arguments_truncated: true,
                arguments_bytes: prompt.arguments_bytes ?? sizes[current]!,
                call: { ...prompt.call, arguments: preview.call.arguments },
            };
        }),
    }));
}
