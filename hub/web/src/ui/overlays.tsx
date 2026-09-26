/**
 * @file overlay primitives: menu, dialog, popover, tooltip, tabs.
 *
 * These are thin wrappers over Radix, and the reason to use Radix at all is
 * specific: the old panel hand-wrote a focus trap and a document-level key
 * handler, and four of its interaction defects came from that one piece of code
 * — a dialog whose initial focus landed on "hide" (D16), a minimised dialog
 * that put itself back (D17), buttons disabled forever after a failure (D18),
 * and a trap that spanned a stack of coexisting `aria-modal` dialogs. Radix
 * supplies focus management, escape handling, outside-click dismissal, roving
 * focus and the ARIA wiring that those bugs were made of.
 *
 * The wrappers exist so the panel has one place where a menu looks like a menu.
 * They deliberately do not hide Radix's parts: a caller that needs `Item` or
 * `Trigger` uses them directly, because a wrapper that re-exported a different
 * vocabulary would be a second API to learn.
 */
import * as DropdownMenu from '@radix-ui/react-dropdown-menu';
import * as DialogPrimitive from '@radix-ui/react-dialog';
import * as PopoverPrimitive from '@radix-ui/react-popover';
import * as TooltipPrimitive from '@radix-ui/react-tooltip';
import * as TabsPrimitive from '@radix-ui/react-tabs';
import { useRef, type ReactNode } from 'react';
import { buttonClass, type ButtonVariant } from './Button.tsx';

/** Shared surface styling, so every floating layer looks the same. */
const SURFACE = 'z-50 rounded-md border border-line bg-raised p-1 shadow-lg '
    + 'outline-none';

// --------------------------------------------------------------- menu --

export const Menu = DropdownMenu.Root;
export const MenuTrigger = DropdownMenu.Trigger;

export function MenuContent({ children, align = 'end' }: {
    children: ReactNode;
    align?: 'start' | 'center' | 'end';
}) {
    return (
        <DropdownMenu.Portal>
            <DropdownMenu.Content align={align} sideOffset={4} className={`${SURFACE} min-w-44`}>
                {children}
            </DropdownMenu.Content>
        </DropdownMenu.Portal>
    );
}

export function MenuItem({ onSelect, danger, disabled, hint, children }: {
    onSelect: () => void;
    danger?: boolean;
    disabled?: boolean;
    /** A short explanation, shown when the item cannot be used. */
    hint?: string | undefined;
    children: ReactNode;
}) {
    return (
        <DropdownMenu.Item
            {...(disabled === undefined ? {} : { disabled })}
            onSelect={onSelect}
            className={`flex cursor-pointer items-center gap-2 rounded px-2 py-1 text-xs outline-none `
                + `data-[disabled]:cursor-not-allowed data-[disabled]:text-ink-faint `
                + `${danger
                    ? 'text-danger data-[highlighted]:bg-danger-soft'
                    : 'text-ink data-[highlighted]:bg-subtle'}`}
        >
            <span>{children}</span>
            {hint && <span className="ml-auto text-xs text-ink-faint">{hint}</span>}
        </DropdownMenu.Item>
    );
}

export function MenuSeparator() {
    return <DropdownMenu.Separator className="my-1 h-px bg-line" />;
}

export function MenuLabel({ children }: { children: ReactNode }) {
    return (
        <DropdownMenu.Label className="px-2 py-1 text-xs uppercase tracking-wide text-ink-faint">
            {children}
        </DropdownMenu.Label>
    );
}

// ------------------------------------------------------------- dialog --

export const Dialog = DialogPrimitive.Root;
export const DialogTrigger = DialogPrimitive.Trigger;
export const DialogClose = DialogPrimitive.Close;

/**
 * The dialog surface.
 *
 * `DialogPrimitive.Content` brings its own focus trap and its own initial
 * focus, which is the fix for D16: the old panel focused the first focusable
 * element in DOM order, which was the header's "hide" button, so pressing Enter
 * on a freshly opened approval dismissed it silently.
 */
export function DialogContent({
    title, description, children, footer, focus = 'first',
}: {
    title: string;
    description?: string | undefined;
    children?: ReactNode;
    footer?: ReactNode;
    /**
     * Where focus lands when the dialog opens.
     *
     * - `first` is Radix's default: the first tabbable element. Right for a
     *   dialog whose first control is harmless, and the reason the destructive
     *   dialogs put Cancel first in the DOM.
     * - `self` focuses the dialog *container*. Nothing is armed — Enter does
     *   nothing — but focus is inside an `aria-modal` element with a title, so
     *   the dialog is still announced. This is what an approval needs: the old
     *   panel's approval modal focused its "hide" button, so Enter dismissed a
     *   decision that had not been made (D16).
     * - `none` leaves focus exactly where it was. Only for a dialog that focuses
     *   something itself, like the palette's filter field.
     */
    focus?: 'first' | 'self' | 'none';
}) {
    const surface = useRef<HTMLDivElement>(null);
    return (
        <DialogPrimitive.Portal>
            <DialogPrimitive.Overlay className="fixed inset-0 z-40 animate-fade bg-scrim" />
            <DialogPrimitive.Content
                ref={surface}
                tabIndex={-1}
                onOpenAutoFocus={(event) => {
                    if (focus === 'none') {
                        event.preventDefault();
                        return;
                    }
                    if (focus === 'self') {
                        event.preventDefault();
                        surface.current?.focus();
                    }
                }}
                className={'fixed left-1/2 top-1/2 z-50 w-[min(32rem,calc(100vw-2rem))] '
                    + '-translate-x-1/2 -translate-y-1/2 animate-pop rounded-lg '
                    + 'border border-line bg-raised p-4 shadow-xl focus:outline-none'}
            >
                <DialogPrimitive.Title className="text-sm font-semibold text-ink">
                    {title}
                </DialogPrimitive.Title>
                {description && (
                    <DialogPrimitive.Description className="mt-1 text-xs text-ink-muted">
                        {description}
                    </DialogPrimitive.Description>
                )}
                <div className="mt-3 text-sm text-ink">{children}</div>
                {footer && (
                    <div className="mt-4 flex items-center justify-end gap-2">{footer}</div>
                )}
            </DialogPrimitive.Content>
        </DialogPrimitive.Portal>
    );
}

/** A button styled to sit in a dialog's footer. */
export function DialogButton({ variant = 'secondary', ...rest }: {
    variant?: ButtonVariant;
} & React.ButtonHTMLAttributes<HTMLButtonElement>) {
    return <button type="button" {...rest} className={buttonClass(variant, 'md')} />;
}

// ------------------------------------------------------------ popover --

export const Popover = PopoverPrimitive.Root;
export const PopoverTrigger = PopoverPrimitive.Trigger;
export const PopoverAnchor = PopoverPrimitive.Anchor;

export function PopoverContent({ children, align = 'end', width }: {
    children: ReactNode;
    align?: 'start' | 'center' | 'end';
    width?: string;
}) {
    return (
        <PopoverPrimitive.Portal>
            <PopoverPrimitive.Content
                align={align}
                sideOffset={4}
                className={`${SURFACE} ${width ?? 'w-72'} p-3`}
            >
                {children}
            </PopoverPrimitive.Content>
        </PopoverPrimitive.Portal>
    );
}

// ------------------------------------------------------------ tooltip --

/**
 * The tooltip provider.
 *
 * Mounted once at the root: Radix needs a single provider above every tooltip,
 * and a tooltip without one throws rather than rendering silently wrong.
 */
export function TooltipProvider({ children }: { children: ReactNode }) {
    return (
        <TooltipPrimitive.Provider delayDuration={400}>
            {children}
        </TooltipPrimitive.Provider>
    );
}

export function Tooltip({ label, children }: { label: string; children: ReactNode }) {
    return (
        <TooltipPrimitive.Root>
            <TooltipPrimitive.Trigger asChild>{children}</TooltipPrimitive.Trigger>
            <TooltipPrimitive.Portal>
                <TooltipPrimitive.Content
                    sideOffset={6}
                    className="z-50 max-w-64 animate-fade rounded bg-tip px-2 py-1 text-xs text-tip-ink shadow"
                >
                    {label}
                    <TooltipPrimitive.Arrow className="fill-tip" />
                </TooltipPrimitive.Content>
            </TooltipPrimitive.Portal>
        </TooltipPrimitive.Root>
    );
}

// --------------------------------------------------------------- tabs --

export const Tabs = TabsPrimitive.Root;

export function TabsList({ children, label }: { children: ReactNode; label: string }) {
    return (
        <TabsPrimitive.List
            aria-label={label}
            className="flex shrink-0 gap-0.5 border-b border-line px-1"
        >
            {children}
        </TabsPrimitive.List>
    );
}

export function TabsTrigger({ value, children }: { value: string; children: ReactNode }) {
    return (
        <TabsPrimitive.Trigger
            value={value}
            className={'rounded-t px-2 py-1 text-xs font-medium text-ink-muted '
                + 'hover:bg-subtle data-[state=active]:bg-subtle '
                + 'data-[state=active]:text-ink focus-visible:outline-2 '
                + 'focus-visible:outline-interactive'}
        >
            {children}
        </TabsPrimitive.Trigger>
    );
}

/**
 * One tab panel.
 *
 * `forceMount` is deliberately not used: a panel that is not selected is not
 * rendered at all, which is the structural fix for defect A3 — the old panel
 * toggled a `hidden` attribute on an element whose author stylesheet set
 * `display: flex`, so every tab ever visited stayed on screen and kept its
 * height.
 */
export function TabsPanel({ value, children }: { value: string; children: ReactNode }) {
    return (
        <TabsPrimitive.Content
            value={value}
            className="min-h-0 flex-1 overflow-y-auto p-2 focus:outline-none"
        >
            {children}
        </TabsPrimitive.Content>
    );
}
