/**
 * @file the theme control.
 *
 * One button that walks light → dark → system. A three-state cycle rather than
 * a two-state switch, because "follow the operating system" is a real
 * preference and a switch has nowhere to put it — which is how a theme control
 * ends up fighting the machine it is running on.
 *
 * The label is the current preference *and* what it resolves to, so the button
 * never has to be pressed to find out what it is doing.
 */
import { useSyncExternalStore } from 'react';
import { Glyph, type GlyphName } from './icons.tsx';
import {
    describeTheme,
    panelTheme,
    type ResolvedTheme,
    type ThemePreference,
} from './theme.ts';

/** What each preference looks like. */
const GLYPH_OF: Record<ThemePreference, GlyphName> = {
    light: 'theme-light',
    dark: 'theme-dark',
    system: 'theme-system',
};

// Module level, so React does not resubscribe on every render.
function subscribe(listener: () => void): () => void {
    return panelTheme().subscribe(listener);
}

export function ThemeToggle() {
    const controller = panelTheme();
    // Two primitive snapshots rather than one object: a snapshot that is a
    // fresh object every call is an infinite render loop in React, and this
    // panel has already paid for that lesson twice.
    const preference: ThemePreference = useSyncExternalStore(
        subscribe, () => controller.preference(),
    );
    const resolved: ResolvedTheme = useSyncExternalStore(subscribe, () => controller.resolved());
    const { label, detail } = describeTheme(preference, resolved);

    return (
        <button
            type="button"
            data-testid="theme-toggle"
            data-theme-preference={preference}
            data-theme-resolved={resolved}
            aria-label={`theme: ${label}`}
            title={`theme: ${detail} — activate for the next of light, dark, system`}
            onClick={() => { controller.cycle(); }}
            className="inline-flex items-center gap-1.5 rounded border border-line px-2 py-1
                text-xs text-ink-muted transition-colors hover:bg-subtle hover:text-ink
                focus-visible:outline-2 focus-visible:outline-offset-1
                focus-visible:outline-interactive"
        >
            <Glyph name={GLYPH_OF[preference]} size="sm" />
            {label}
        </button>
    );
}
