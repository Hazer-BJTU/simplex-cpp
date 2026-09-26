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
    primary: 'bg-slate-900 text-white hover:bg-slate-700 disabled:bg-slate-400',
    secondary: 'border border-slate-300 bg-white text-slate-700 hover:bg-slate-100 '
        + 'disabled:text-slate-400 disabled:hover:bg-white',
    ghost: 'text-slate-600 hover:bg-slate-100 disabled:text-slate-300 disabled:hover:bg-transparent',
    danger: 'border border-rose-300 bg-white text-rose-700 hover:bg-rose-50 '
        + 'disabled:border-slate-200 disabled:text-slate-400 disabled:hover:bg-white',
};

/** Shared classes, so a button in a menu and a button in a header match. */
export function buttonClass(
    variant: ButtonVariant = 'secondary',
    size: 'sm' | 'md' = 'sm',
): string {
    const sizing = size === 'md' ? 'px-3 py-1.5 text-sm' : 'px-2 py-1 text-xs';
    return `inline-flex items-center gap-1 rounded font-medium transition-colors `
        + `disabled:cursor-not-allowed focus-visible:outline-2 focus-visible:outline-offset-1 `
        + `focus-visible:outline-slate-500 ${sizing} ${VARIANTS[variant]}`;
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
                + `focus-visible:outline-slate-500 ${VARIANTS[variant]} ${className}`}
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
        neutral: 'bg-slate-100 text-slate-600 ring-slate-300',
        info: 'bg-sky-100 text-sky-900 ring-sky-300',
        ok: 'bg-emerald-100 text-emerald-900 ring-emerald-300',
        warn: 'bg-amber-100 text-amber-900 ring-amber-300',
        bad: 'bg-rose-100 text-rose-900 ring-rose-300',
    } as const;
    return (
        <span
            title={title}
            className={`inline-flex items-center gap-1 rounded-full px-2 py-0.5 text-[11px] `
                + `font-medium ring-1 ring-inset ${tones[tone]}`}
        >
            {children}
        </span>
    );
}
