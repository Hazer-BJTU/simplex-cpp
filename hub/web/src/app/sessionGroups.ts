import type { SessionDescription } from '../../../ui/index.ts';

/** A flat family: descendants keep their direct parent on their own row. */
export interface SubagentGroup {
    root: string;
    sessions: SessionDescription[];
    pending: number;
}

/** Resolve missing parents and malformed cycles without hiding any session. */
export function sessionRoot(id: string, sessions: ReadonlyMap<string, SessionDescription>): string {
    const visited = new Map<string, number>();
    let current = id;
    while (true) {
        const cycleStart = visited.get(current);
        if (cycleStart !== undefined) {
            return [...visited.keys()].slice(cycleStart).sort()[0]!;
        }
        visited.set(current, visited.size);
        const session = sessions.get(current);
        if (session?.kind !== 'headless' || !session.subagent?.parent) return current;
        current = session.subagent.parent;
    }
}

/** Keep main ordering intact; prioritize only the selected family among subagents. */
export function groupSessions(sessions: ReadonlyMap<string, SessionDescription>, selected: string | null) {
    const ordered = [...sessions.values()].sort((a, b) => (
        (b.created_at ?? '').localeCompare(a.created_at ?? '')
        || a.session_id.localeCompare(b.session_id)
    ));
    const main = ordered.filter(session => session.kind !== 'headless');
    const families = new Map<string, SubagentGroup>();
    for (const session of ordered) {
        if (session.kind !== 'headless') continue;
        const root = sessionRoot(session.session_id, sessions);
        let group = families.get(root);
        if (!group) {
            group = { root, sessions: [], pending: 0 };
            families.set(root, group);
        }
        group.sessions.push(session);
        group.pending += session.confirmations.filter(prompt => prompt.settled_at === null).length;
    }
    const selectedRoot = selected ? sessionRoot(selected, sessions) : null;
    const groups = [...families.values()].sort((a, b) => (
        Number(b.root === selectedRoot) - Number(a.root === selectedRoot)
        || a.root.localeCompare(b.root)
    ));
    return { main, groups };
}
