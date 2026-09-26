/**
 * @file the panel's icon set.
 *
 * One module names every glyph, so "which icon means *stop*" is answered once
 * rather than at each call site. The old panel drew eight unlabelled dots from
 * three different idioms and no size rule; the icons here share a stroke width,
 * a three-step size scale, and one rule about meaning: **a glyph is decorative
 * and the accessible name always comes from the control it sits in.**
 *
 * That rule is why `aria-hidden` is baked into `Glyph` rather than passed at
 * each use. An icon that carried its own name would be announced twice inside a
 * labelled button ("Start, Play"), and the second name is always the worse one.
 * The panel's `IconButton` makes `label` mandatory for the same reason.
 *
 * Icons are imported from one package and re-exported from nowhere else: a
 * component that reaches for `lucide-react` directly is the first step back to
 * three idioms, and a test scans for it (`panel-polish.test.js`).
 */
import {
    Activity,
    ArrowDown,
    ChevronDown,
    ChevronRight,
    CircleAlert,
    CircleCheck,
    CircleX,
    Command,
    Copy,
    Check,
    Ellipsis,
    FileText,
    Inbox,
    Info,
    List,
    LoaderCircle,
    MessageSquare,
    Monitor,
    Moon,
    PanelRight,
    Paperclip,
    Plus,
    Play,
    Power,
    RefreshCw,
    RotateCcw,
    Send,
    Settings2,
    ShieldAlert,
    ShieldQuestion,
    Square,
    Sun,
    Terminal,
    Trash2,
    TriangleAlert,
    Wifi,
    WifiOff,
    X,
    Zap,
    type LucideIcon,
} from 'lucide-react';

/** Every meaning the panel draws. The key is the meaning, not the picture. */
export const GLYPHS = {
    // shell and navigation
    sessions: List,
    'new-session': Plus,
    inspector: PanelRight,
    command: Command,
    more: Ellipsis,
    close: X,
    refresh: RefreshCw,
    'chevron-right': ChevronRight,
    'chevron-down': ChevronDown,
    latest: ArrowDown,

    // process lifecycle
    start: Play,
    stop: Square,
    restart: RotateCcw,
    shutdown: Power,
    'force-kill': Zap,
    delete: Trash2,

    // signals and panes
    status: Activity,
    options: Settings2,
    cancel: CircleX,

    // composer
    send: Send,
    attach: Paperclip,

    // approvals
    approval: ShieldAlert,
    unverified: ShieldQuestion,

    // feedback
    ok: CircleCheck,
    warning: TriangleAlert,
    error: CircleAlert,
    info: Info,
    spinner: LoaderCircle,
    online: Wifi,
    offline: WifiOff,

    // content
    copy: Copy,
    copied: Check,
    output: FileText,
    logs: Terminal,

    // theme
    'theme-light': Sun,
    'theme-dark': Moon,
    'theme-system': Monitor,

    // empty states
    'empty-session': MessageSquare,
    'empty-list': Inbox,
} satisfies Record<string, LucideIcon>;

/** The meanings, which is also the complete list of what `Glyph` accepts. */
export type GlyphName = keyof typeof GLYPHS;

/**
 * Three sizes, and no fourth.
 *
 * `sm` is for a glyph inside a line of text, `md` for an icon button or a menu
 * row, `lg` for an empty state. Anything that wants a different size is really
 * asking for a different size *scale*, which is a design decision, not a prop.
 */
export type GlyphSize = 'sm' | 'md' | 'lg';

const SIZES: Record<GlyphSize, number> = { sm: 12, md: 14, lg: 20 };

/**
 * One icon.
 *
 * `strokeWidth` is fixed rather than defaulted per call: lucide's own default
 * of 2 is too heavy at 12px beside 12px text, and mixing weights is exactly
 * what "three idioms" looked like the first time.
 */
export function Glyph({ name, size = 'md', className = '' }: {
    name: GlyphName;
    size?: GlyphSize;
    className?: string;
}) {
    const Icon = GLYPHS[name];
    const decoration = name === 'spinner' ? 'animate-spin' : '';
    return (
        <Icon
            aria-hidden
            focusable="false"
            size={SIZES[size]}
            strokeWidth={1.75}
            className={`shrink-0 ${decoration} ${className}`}
        />
    );
}
