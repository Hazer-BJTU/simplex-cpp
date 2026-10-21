/** Draft-local skill snapshots, isolated from late responses after session changes. */
import { useEffect, useState } from 'react';
import type { SkillDocument, SkillList } from '../../../ui/index.ts';
import type { RestClient } from '../lib/rest.ts';
import { expandSkills, referencedSkillIds, skillMarkers } from './skillInput.ts';

export function useSkills(draft: string, session: string | null, rest: RestClient) {
    const [skills, setSkills] = useState<Record<string, SkillDocument>>({});
    const [errors, setErrors] = useState<Record<string, string>>({});
    const [catalog, setCatalog] = useState<SkillList>({ skills: [], errors: [] });
    const [libraryError, setLibraryError] = useState('');
    const [refresh, setRefresh] = useState(0);
    const [libraryOpen, setLibraryOpen] = useState(false);
    const ids = referencedSkillIds(draft);
    let error = '';
    try { skillMarkers(draft); }
    catch (e) { error = (e as Error).message; }
    const key = JSON.stringify(ids);

    useEffect(() => { setSkills({}); setErrors({}); setLibraryOpen(false); }, [session]);
    const wantsLibrary = libraryOpen || draft.includes('@{skill:');
    useEffect(() => {
        if (!wantsLibrary) return;
        const controller = new AbortController();
        setLibraryError('');
        void rest.request<SkillList>('GET', '/api/skills', { signal: controller.signal })
            .then(list => { if (!controller.signal.aborted) setCatalog(list); })
            .catch(e => { if (!controller.signal.aborted) setLibraryError((e as Error).message); });
        return () => controller.abort();
    }, [wantsLibrary, libraryOpen, refresh, session, rest]);

    useEffect(() => {
        const wanted = JSON.parse(key) as string[];
        const controller = new AbortController();
        // Remove detached snapshots so deleting a marker releases its content.
        setSkills(previous => Object.fromEntries(Object.entries(previous).filter(([id]) => wanted.includes(id))));
        setErrors(previous => Object.fromEntries(Object.entries(previous).filter(([id]) => wanted.includes(id))));
        for (const id of wanted) {
            if (Object.hasOwn(skills, id) || Object.hasOwn(errors, id)) continue;
            void rest.request<SkillDocument>('GET', `/api/skills/${encodeURIComponent(id)}`, { signal: controller.signal })
                .then(skill => {
                    if (controller.signal.aborted) return;
                    if (skill.id !== id || typeof skill.text !== 'string' || !skill.text.trim()
                        || new TextEncoder().encode(skill.text).length > 64 * 1024) {
                        throw new Error('Invalid skill response');
                    }
                    setSkills(previous => ({ ...previous, [id]: skill }));
                }).catch(e => {
                    if (!controller.signal.aborted) setErrors(previous => ({ ...previous, [id]: (e as Error).message }));
                });
        }
        return () => controller.abort();
        // Snapshot arrivals do not restart sibling requests. A new marker list,
        // session, or explicit refresh is the only reason to start another read.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [key, session, refresh, rest]);

    let expanded = draft;
    if (!error) {
        error = ids.map(id => Object.hasOwn(errors, id) ? errors[id] : '').find(Boolean) ?? '';
        if (!error) {
            const missing = ids.find(id => !Object.hasOwn(skills, id));
            if (missing) error = `Loading skill ${missing}…`;
            try { if (!error) expanded = draft.includes('@{skill:') ? expandSkills(draft, skills) : draft; }
            catch (e) { error = (e as Error).message; }
        }
    }
    return { skills, catalog, error, expanded, libraryError, libraryOpen, setLibraryOpen,
        reload: () => { setRefresh(n => n + 1); },
        refreshSnapshots: () => { setSkills({}); setErrors({}); setRefresh(n => n + 1); },
        restore: (value: Record<string, SkillDocument>) => { setSkills(value); setErrors({}); },
        ids };
}
