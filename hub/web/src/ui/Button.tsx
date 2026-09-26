/**
 * @file buttons.
 *
 * Two components rather than one with a flag, because they are used for
 * different things and the difference is a rule rather than a style: a labelled
 * button says what it does, an icon button has to say what it does some other
 * way, and the old panel's ten peer buttons of equal weight is what happens
 * when that rule is missing.
 *
 * The variants encode the weight the design asks for — one solid button for the
 * primary action of a view, quiet secondary buttons, and danger reserved for
 * the things that cannot be undone. The old panel had `Force kill` in red and
 * `Delete` as a ghost, which is the weighting exactly backwards.
 */
import type { ComponentPropsWithRef, ReactNode } from 'react';

/** How much weight a button carries. */
export type ButtonVariant = 'primary' | 'secondary' | 'ghost' | 'danger';

const VARIANTS: Record<ButtonVariant, string> = {
    primary: 'bg-accent text-accent-ink hover:bg-accent-hover disabled:bg-line-strong',
    secondary: 'border border-line-strong bg-surface text-ink hover:bg-subtle '
        + 'disabled:text-ink-faint disabled:hover:bg-surface',
    ghost: 'text-ink-muted hover:bg-subtle disabled:text-ink-faint disabled:hover:bg-transparent',
    danger: 'border border-danger-line bg-surface text-danger hover:bg-danger-soft '
        + 'disabled:border-line disabled:text-ink-faint disabled:hover:bg-surface',
};

/** Shared classes, so a button in a menu and a button in a header match. */
export function buttonClass(
    variant: ButtonVariant = 'secondary',
    size: 'sm' | 'md' = 'sm',
): string {
    const sizing = size === 'md' ? 'px-3 py-1.5 text-sm' : 'px-2 py-1 text-xs';
    return `inline-flex items-center gap-1 rounded font-medium transition-colors `
        + `disabled:cursor-not-allowed focus-visible:outline-2 focus-visible:outline-offset-1 `
        + `focus-visible:outline-interactive ${sizing} ${VARIANTS[variant]}`;
}

/**
 * Props for a labelled button.
 *
 * `ComponentPropsWithRef` rather than `ButtonHTMLAttributes`, because Radix's
 * `asChild` clones its child with a ref: without one, a menu trigger or a
 * tooltip trigger silently loses its positioning and its aria wiring. React 19
 * passes `ref` as an ordinary prop to a function component, so spreading the
 * rest onto the element is all it takes.
 */
export interface ButtonProps extends ComponentPropsWithRef<'button'> {
    variant?: ButtonVariant;
    size?: 'sm' | 'md';
    /** Shown before the label; decorative, so it is hidden from the a11y tree. */
    icon?: ReactNode;
}

export function Button({
    variant = 'secondary',
    size = 'sm',
    icon,
    className = '',
    children,
    ...rest
}: ButtonProps) {
    return (
        <button type="button" {...rest} className={`${buttonClass(variant, size)} ${className}`}>
            {icon}
            {children}
        </button>
    );
}

export interface IconButtonProps extends ComponentPropsWithRef<'button'> {
    /** Required: an icon alone has no accessible name without it. */
    label: string;
    variant?: ButtonVariant;
    children: ReactNode;
}

/**
 * A button whose only visible content is an icon.
 *
 * `label` is required and becomes both the accessible name and the tooltip, so
 * an icon button cannot be built without an answer to "what does this do" —
 * which is the defect the old panel's eight unlabelled dots represented.
 */
export function IconButton({
    label,
    variant = 'ghost',
    className = '',
    children,
    ...rest
}: IconButtonProps) {
    return (
        <button
            type="button"
            {...rest}
            aria-label={label}
            title={label}
            className={`inline-flex h-7 w-7 items-center justify-center rounded transition-colors `
                + `disabled:cursor-not-allowed focus-visible:outline-2 focus-visible:outline-offset-1 `
                + `focus-visible:outline-interactive ${VARIANTS[variant]} ${className}`}
        >
            {children}
        </button>
    );
}

/** A small state chip. Tone carries meaning; the text carries it again. */
export function Badge({ tone = 'neutral', title, children }: {
    tone?: 'neutral' | 'info' | 'ok' | 'warn' | 'bad';
    title?: string | undefined;
    children: ReactNode;
}) {
    const tones = {
        neutral: 'bg-subtle text-ink-muted ring-line',
        info: 'bg-info-soft text-info ring-info-line',
        ok: 'bg-ok-soft text-ok ring-ok-line',
        warn: 'bg-warn-soft text-warn ring-warn-line',
        bad: 'bg-danger-soft text-danger ring-danger-line',
    } as const;
    return (
        <span
            title={title}
            className={`inline-flex items-center gap-1 rounded-full px-2 py-0.5 text-xs `
                + `font-medium ring-1 ring-inset ${tones[tone]}`}
        >
            {children}
        </span>
    );
}
