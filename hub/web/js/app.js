/**
 * @file panel application: layout wiring, session list, transcript, composer,
 * inspector, modals.
 *
 * This module owns every DOM reference. It subscribes to the store and appends
 * one element per arriving message instead of re-rendering the transcript, and
 * it never treats a successful `send` as delivery: request outcomes are shown
 * only from the hub's `request` messages.
 */
import { ApiError, createPanelSocket, createRest, createTokenStore } from './api.js';
import { createStore, RUN_END_EVENTS, RUN_START_EVENTS } from './state.js';
import * as R from './render.js';

const THEME_KEY = 'simplex-hub-theme';
const LOG_REQUEST_LIMIT = 200;

/** Friendly text for the hub's machine-readable error codes. */
const ERROR_TEXT = {
    unknown_session: 'the hub does not know that session',
    input_not_sent: 'the payload was not sent',
    signal_not_sent: 'the signal was not sent',
    unknown_confirmation: 'that confirmation is no longer open',
    bad_json: 'the hub could not parse the panel message',
    unsupported_version: 'the panel and hub protocol versions differ',
    session_exists: 'a session with that id already exists',
    invalid_session: 'the session id or spec was rejected',
    session_busy: 'the session is busy: stop the worker and let it disconnect first',
    worker_action_failed: 'the worker action failed',
    confirmation_rejected: 'the confirmation could not be decided',
    not_found: 'not found',
    internal_error: 'hub internal error',
};

/** Confirmation outcome phases explained for the operator. */
const OUTCOME_TEXT = {
    decided: 'a decision was settled',
    disconnected: 'the worker disconnected before a decision arrived',
    deadline: 'the confirmation deadline passed before a decision',
    shutdown: 'the hub shut down while the prompt was open',
};

const $ = (id) => document.getElementById(id);

/** dom id -> property name on `ui` (`act-start` -> `actStart`). */
const uiKey = (id) => id.replace(/-([a-z])/g, (_, letter) => letter.toUpperCase());

/** Everything the app owns; filled by init(). */
const ui = {};
/** Session ids with hidden (minimized) confirmation modals. */
const hiddenConfirmations = new Set();

const state = {
    store: null,
    rest: null,
    socket: null,
    tokens: null,
    parts: new Map(),
    pendingOptions: new Map(),
    snapshots: new Map(),
    inspectorTab: 'status',
    centerTab: 'chat',
    requestElements: new Map(),
    currentGroup: null,
    modals: [],
    createPending: null,
    rawDirty: true,
    versionWarned: false,
    authProbeRunning: false,
};

// ------------------------------------------------------------------ theme --

function initialTheme() {
    try {
        const saved = globalThis.localStorage?.getItem(THEME_KEY);
        if (saved === 'dark' || saved === 'light') return saved;
    } catch { /* storage unavailable */ }
    return globalThis.matchMedia?.('(prefers-color-scheme: dark)')?.matches ? 'dark' : 'light';
}

function applyTheme(theme) {
    document.documentElement.dataset.theme = theme;
    if (ui.themeToggle) {
        ui.themeToggle.textContent = theme === 'dark' ? 'light' : 'dark';
        ui.themeToggle.setAttribute('aria-label',
            theme === 'dark' ? 'Switch to light theme' : 'Switch to dark theme');
    }
    try {
        globalThis.localStorage?.setItem(THEME_KEY, theme);
    } catch { /* storage unavailable */ }
}

// -------------------------------------------------------------- toasts ----

function toast(tone, text, { actionLabel, onAction, timeoutMs = 9000 } = {}) {
    const body = R.el('span', { class: 'toast-body', text });
    const close = R.el('button', {
        class: 'toast-close',
        text: '×',
        attrs: { type: 'button', 'aria-label': 'Dismiss notification' },
        on: { click: () => node.remove() },
    });
    const node = R.el('div', { class: `toast toast-${tone}` }, [body]);
    if (actionLabel && onAction) {
        node.append(R.el('button', {
            class: 'btn btn-sm',
            text: actionLabel,
            attrs: { type: 'button' },
            on: {
                click: () => {
                    onAction();
                    node.remove();
                },
            },
        }));
    }
    node.append(close);
    ui.toastRoot.append(node);
    while (ui.toastRoot.children.length > 6) ui.toastRoot.firstChild.remove();
    if (timeoutMs > 0) globalThis.setTimeout(() => node.remove(), timeoutMs);
}

// -------------------------------------------------------------- modals ----

function focusables(root) {
    return [...root.querySelectorAll(
        'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])')]
        .filter((node) => !node.disabled && node.offsetParent !== null);
}

function openModal(id, element, { onEscape, stateKey = null, focusFirst = true } = {}) {
    const existingIndex = state.modals.findIndex((modal) => modal.id === id);
    if (existingIndex !== -1) {
        const [existing] = state.modals.splice(existingIndex, 1);
        existing.el.remove();
    }
    const entry = { id, el: element, onEscape, stateKey };
    state.modals.push(entry);
    ui.modalStack.append(element);
    ui.modalRoot.hidden = false;
    if (focusFirst) {
        const target = focusables(element)[0] ?? element;
        globalThis.setTimeout(() => target.focus(), 0);
    }
    return entry;
}

function closeModal(id) {
    const index = state.modals.findIndex((modal) => modal.id === id);
    if (index === -1) return false;
    const [entry] = state.modals.splice(index, 1);
    entry.el.remove();
    if (state.modals.length === 0) ui.modalRoot.hidden = true;
    return true;
}

function modalOpen(id) {
    return state.modals.some((modal) => modal.id === id);
}

function trapFocus(event) {
    const scope = ui.modalStack;
    const nodes = focusables(scope);
    if (nodes.length === 0) {
        event.preventDefault();
        return;
    }
    const first = nodes[0];
    const last = nodes[nodes.length - 1];
    const active = document.activeElement;
    if (event.shiftKey && (active === first || !scope.contains(active))) {
        event.preventDefault();
        last.focus();
    } else if (!event.shiftKey && (active === last || !scope.contains(active))) {
        event.preventDefault();
        first.focus();
    }
}

function onDocumentKeydown(event) {
    if (state.modals.length === 0) return;
    if (event.key === 'Escape') {
        event.preventDefault();
        const top = state.modals[state.modals.length - 1];
        if (typeof top.onEscape === 'function') top.onEscape();
        else closeModal(top.id);
        return;
    }
    if (event.key === 'Tab') trapFocus(event);
}

// --------------------------------------------------------------- ticker ---

function startCountdownTicker() {
    const tick = () => {
        for (const node of document.querySelectorAll('[data-deadline]')) {
            const deadline = node.dataset.deadline;
            if (!deadline) {
                node.textContent = 'no deadline reported';
                continue;
            }
            const text = R.countdownText(deadline);
            node.textContent = text;
            node.classList.toggle('expired', text === 'deadline passed');
        }
    };
    tick();
    globalThis.setInterval(tick, 1000);
}

// ----------------------------------------------------------------- auth ---

function requireToken(message) {
    ui.tokenOverlay.hidden = false;
    ui.tokenError.textContent = message ?? '';
    ui.tokenInput.focus();
}

function onUnauthorized() {
    state.store.setPanel({ authRequired: true });
    requireToken('The hub rejected the request: a valid panel token is required.');
}

async function submitToken(event) {
    event.preventDefault();
    const value = ui.tokenInput.value.trim();
    if (!value) {
        ui.tokenError.textContent = 'Enter the token printed by the hub operator.';
        return;
    }
    state.tokens.set(value);
    ui.tokenInput.value = '';
    ui.tokenOverlay.hidden = true;
    state.store.setPanel({ authRequired: false });
    // REST first: it reports 401 unambiguously, unlike a browser WebSocket.
    try {
        const meta = await state.rest.meta();
        state.store.setHub(meta);
        const sessions = await state.rest.sessions();
        state.store.setSessions(sessions.sessions ?? []);
    } catch (error) {
        if (error instanceof ApiError && error.unauthorized) {
            state.tokens.clear();
            requireToken('That token was rejected.');
            return;
        }
    }
    state.socket.reconnectNow();
}

// ------------------------------------------------------------ bootstrap ---

async function bootstrap() {
    state.rest = createRest({ token: () => state.tokens.get(), onUnauthorized });
    try {
        const meta = await state.rest.meta();
        state.store.setHub(meta);
    } catch (error) {
        if (error instanceof ApiError && error.unauthorized) return; // overlay shown
        toast('warn', `hub metadata unavailable over REST: ${error.message}`);
    }
    try {
        const sessions = await state.rest.sessions();
        state.store.setSessions(sessions.sessions ?? []);
    } catch (error) {
        if (!(error instanceof ApiError && error.unauthorized)) {
            toast('warn', `session list unavailable over REST: ${error.message}`);
        }
    }
    connectSocket();
}

function connectSocket() {
    state.socket?.close();
    state.socket = createPanelSocket({
        token: () => state.tokens.get(),
        onMessage: onHubMessage,
        onState: onSocketState,
    });
    state.socket.connect();
}

function onSocketState(socketState) {
    state.store.setPanel({
        state: socketState.state,
        attempt: socketState.attempt ?? 0,
        nextDelayMs: socketState.nextDelayMs ?? null,
        error: socketState.error ?? null,
    });
    if (socketState.state === 'rejected' && !state.authProbeRunning) {
        // A close before `welcome` is what a token rejection looks like here;
        // REST can tell the two apart, so ask it.
        state.authProbeRunning = true;
        state.rest.meta()
            .catch((error) => {
                if (error instanceof ApiError && error.unauthorized) onUnauthorized();
            })
            .finally(() => { state.authProbeRunning = false; });
    }
}

// ------------------------------------------------------- hub messages -----

function onHubMessage(message, raw) {
    if (!message || typeof message !== 'object') {
        toast('warn', 'the hub sent a message that is not a JSON object; it was ignored');
        void raw;
        return;
    }
    if (message.v !== undefined && message.v !== 1 && !state.versionWarned) {
        state.versionWarned = true;
        toast('warn', `the hub speaks panel protocol v${message.v}; this panel expects v1`);
    }
    switch (message.type) {
        case 'welcome':
            state.store.applyWelcome(message);
            // Pick a session to show: an explicit ?session= link, the current
            // selection, otherwise the first one — a freshly opened panel should
            // be useful without a click.
            selectSession(preferredSession(state.store.ids()), { force: true });
            return;
        case 'sessions':
            state.store.setSessions(message.sessions ?? []);
            return;
        case 'session':
            state.store.upsertSession(message.session);
            return;
        case 'session_removed':
            state.store.removeSession(message.session);
            cleanupSession(message.session);
            toast('info', `session ${message.session} was removed`);
            return;
        case 'subscribed':
            state.store.applySubscribed(message);
            return;
        case 'event':
            state.store.applyEvent(message);
            return;
        case 'snapshot':
            state.store.applySnapshot(message);
            return;
        case 'confirmation':
            state.store.applyConfirmation(message);
            return;
        case 'process':
            state.store.applyProcess(message);
            return;
        case 'connection':
            state.store.applyConnection(message);
            return;
        case 'request':
            state.store.applyRequest(message);
            return;
        case 'logs':
            state.store.applyLogs(message);
            return;
        case 'created':
            state.createPending?.setBusy();
            state.createPending = null;
            closeModal('new-session');
            state.store.upsertSession(message.session);
            selectSession(message.session.session_id);
            toast('ok', `session ${message.session.session_id} created`, {
                actionLabel: 'Start worker',
                onAction: () => state.socket.send({
                    type: 'worker', session: message.session.session_id, action: 'start',
                }),
            });
            return;
        case 'accepted':
            onAccepted(message);
            return;
        case 'error':
            onHubError(message);
            return;
        case 'pong':
            return;
        default:
            // Unknown message types are ignored by design (forward compatibility).
            return;
    }
}

function onAccepted(message) {
    if (message.action === 'input') {
        toast('info', `payload handed to the panel socket · request ${R.shortId(message.request_id)}`
            + ' — tracked by the hub, but "sent" is not "executed"');
        return;
    }
    if (message.action === 'signal') {
        toast('info', `signal "${message.operation}" accepted by the hub`);
        return;
    }
    if (message.action === 'worker') {
        toast('info', `worker action "${message.action}" accepted by the hub`);
        return;
    }
    if (message.action === 'confirmation') {
        toast('info', 'decision sent on the confirmation socket'
            + ' — an approval does not prove the call ran');
    }
}

function onHubError(message) {
    const code = typeof message.error === 'string' ? message.error : 'unknown_error';
    const detail = typeof message.message === 'string' ? message.message : '';
    toast('error', `${code}: ${detail || ERROR_TEXT[code] || 'the hub rejected the message'}`);
    if ((code === 'session_exists' || code === 'invalid_session') && state.createPending) {
        state.createPending.setError(detail || ERROR_TEXT[code]);
    }
    if (code === 'unknown_session') {
        state.rest?.sessions()
            .then((result) => state.store.setSessions(result.sessions ?? []))
            .catch(() => { /* the socket will resync */ });
    }
}

function cleanupSession(sessionId) {
    state.parts.delete(sessionId);
    state.pendingOptions.delete(sessionId);
    state.snapshots.delete(sessionId);
    for (const modal of [...state.modals]) {
        const prompt = modal.promptData;
        if (prompt?.session_id === sessionId) closeModal(modal.id);
    }
    if (state.store.selected() === null) renderSelection();
}

// ------------------------------------------------------------- selection --

/**
 * Session to show when the panel connects: an explicit `?session=` link wins,
 * then the previous selection, then the first session in the list.
 */
function preferredSession(ids) {
    const requested = new URLSearchParams(window.location.search).get('session');
    if (requested && ids.includes(requested)) return requested;
    const current = state.store.selected();
    if (current && ids.includes(current)) return current;
    return ids[0] ?? null;
}

function selectSession(sessionId, { force = false } = {}) {
    const previous = state.store.selected();
    if (previous === sessionId && !force) return;
    if (previous && previous !== sessionId && state.socket?.isOpen()) {
        state.socket.send({ type: 'unsubscribe', session: previous });
    }
    state.store.setSelected(sessionId);
    if (sessionId) {
        // `force` is what re-subscribes after a reconnect: the hub has a new
        // socket, so the previous subscription no longer exists.
        const sent = state.socket?.send({
            type: 'subscribe',
            session: sessionId,
            since: state.store.lastSeq(sessionId),
        });
        if (!sent) {
            toast('warn', 'the panel socket is not connected; the session will subscribe after reconnect');
        }
    }
}

// ---------------------------------------------------------- store render --

function onStoreChange(change) {
    switch (change.type) {
        case 'hub':
            renderHub();
            return;
        case 'panel':
            renderPanelBadge();
            return;
        case 'sessions':
            renderSessionList();
            return;
        case 'session':
        case 'connection':
        case 'process':
            renderSessionRow(change.sessionId);
            if (change.sessionId === state.store.selected()) {
                renderHeader();
                if (change.type === 'process') renderInspector();
            }
            return;
        case 'selected':
            renderSelection();
            return;
        case 'subscribed':
            if (change.sessionId === state.store.selected()) {
                renderTimelineFull();
                renderRawFull();
            }
            renderSessionRow(change.sessionId);
            renderInspector();
            return;
        case 'snapshot':
            if (change.sessionId === state.store.selected()) {
                renderTimelineFull();
                renderRawFull();
            }
            return;
        case 'event':
            handleEventChange(change);
            return;
        case 'request':
            handleRequestChange(change);
            return;
        case 'request-update':
            handleRequestUpdate(change);
            return;
        case 'note':
            if (change.sessionId === state.store.selected()) {
                appendTimelineItem(change.item);
                stickScroll();
            }
            return;
        case 'logs':
            if (change.sessionId === state.store.selected()
                && state.inspectorTab === 'logs') {
                renderInspector();
            }
            return;
        case 'confirmation-open':
            renderSessionRow(change.sessionId);
            openConfirmationModal(change.prompt);
            if (change.sessionId === state.store.selected()) renderHeader();
            return;
        case 'confirmation-closed':
            closeConfirmationModal(change);
            renderSessionRow(change.sessionId);
            if (change.sessionId === state.store.selected()) renderHeader();
            return;
        case 'session-removed':
            renderSessionList();
            return;
        default:
    }
}

function handleEventChange(change) {
    const { envelope } = change;
    if (change.sessionId === state.store.selected()) {
        appendTimelineItem(change.item);
        stickScroll();
        if (isRawVisible()) appendRawItem(change.item);
        else state.rawDirty = true;
        if (envelope.event === 'status' || envelope.event === 'ready' || envelope.event === 'options') {
            renderInspector();
        }
        renderHeader();
        updateComposerHint();
    }
    renderSessionRow(change.sessionId);
}

function handleRequestChange(change) {
    if (change.sessionId === state.store.selected()) {
        appendTimelineItem(change.item);
        stickScroll();
    }
    renderSessionRow(change.sessionId);
}

function handleRequestUpdate(change) {
    if (change.sessionId !== state.store.selected()) return;
    const previous = state.requestElements.get(change.request.request_id);
    const replacement = R.renderRequestChip(change.request);
    state.requestElements.set(change.request.request_id, replacement);
    if (previous && previous.parentNode) previous.replaceWith(replacement);
    else appendTimelineItem(change.item);
    renderSessionRow(change.sessionId);
}

// ------------------------------------------------------------- sidebar ----

function renderHub() {
    const hub = state.store.hub();
    ui.hubName.textContent = hub?.name ? `${hub.name}` : 'simplex hub';
    const bits = [];
    if (hub?.version) bits.push(`v${hub.version}`);
    if (hub?.protocol?.version) bits.push(`panel v${hub.protocol.version}`);
    if (hub?.launcher?.kind) bits.push(hub.launcher.kind);
    if (hub?.mock?.enabled) bits.push('mock provider');
    ui.hubVersion.textContent = bits.join(' · ');
}

function renderPanelBadge() {
    const panel = state.store.panel();
    const labels = {
        idle: ['offline', 'badge-muted'],
        connecting: ['connecting…', 'badge-warn'],
        open: ['connected', 'badge-ok'],
        reconnecting: ['reconnecting…', 'badge-warn'],
        rejected: ['rejected — retrying', 'badge-error'],
        closed: ['closed', 'badge-muted'],
    };
    const [label, className] = labels[panel.state] ?? ['unknown', 'badge-muted'];
    const detail = panel.nextDelayMs ? ` · retry in ${Math.round(panel.nextDelayMs / 1000)}s` : '';
    ui.panelBadge.textContent = label;
    ui.panelBadge.className = `badge ${className}`;
    ui.panelBadge.title = `panel socket: ${panel.state}${detail}${panel.error ? ` (${panel.error})` : ''}`;
}

function sessionDot(session) {
    if (!session) return ['dot-muted', 'unknown session'];
    if (session.connected) return ['dot-ok', 'worker connected'];
    const process = session.process;
    if (process && ['starting', 'running', 'stopping'].includes(process.state)) {
        return ['dot-warn', `worker process ${process.state}, event connection not open`];
    }
    if (process && (process.state === 'failed'
        || (typeof process.exit_code === 'number' && process.exit_code !== 0))) {
        return ['dot-error', `worker process failed (state ${process.state}, code ${process.exit_code ?? '—'})`];
    }
    if (process) return ['dot-muted', `worker process ${process.state}`];
    return ['dot-muted', 'idle: no worker process recorded'];
}

function sessionBadges(session) {
    const badges = [];
    const id = session.session_id;
    const stats = state.store.stats(id);
    const loop = state.store.loop(id);
    const statusData = state.store.statusData(id);
    if (stats.confirmations > 0 || (session.confirmations?.length ?? 0) > 0) {
        const count = Math.max(stats.confirmations, session.confirmations?.length ?? 0);
        badges.push(R.el('span', { class: 'mini-badge mb-confirm', text: `${count} confirmation(s)` }));
    }
    if (stats.unknownRequests > 0) {
        badges.push(R.el('span', {
            class: 'mini-badge mb-unknown',
            text: `${stats.unknownRequests} unknown outcome`,
            title: 'a payload was sent but its admission was never observed',
        }));
    }
    const gaps = (session.stats?.gaps ?? 0) + stats.gaps;
    if (gaps > 0) {
        badges.push(R.el('span', {
            class: 'mini-badge mb-gap',
            text: `${gaps} gap(s)`,
            title: 'worker event sequence numbers skipped',
        }));
    }
    if (statusData?.storage_failed === true) {
        badges.push(R.el('span', { class: 'mini-badge mb-storage', text: 'storage_failed' }));
    }
    if (loop?.phase === 'tools' || loop?.phase === 'blocked') {
        badges.push(R.el('span', {
            class: 'mini-badge mb-phase',
            text: `recovery: ${loop.phase}`,
            title: 'the worker is resuming from a persisted checkpoint',
        }));
    }
    return badges;
}

function renderSessionRow(sessionId) {
    const row = ui.sessionList.querySelector(`[data-session="${CSS.escape(sessionId)}"]`);
    if (!row) {
        renderSessionList();
        return;
    }
    const session = state.store.get(sessionId);
    if (!session) return;
    const [dotClass, dotTitle] = sessionDot(session);
    const dot = row.querySelector('.dot');
    dot.className = `dot ${dotClass}`;
    dot.title = dotTitle;
    row.querySelector('.row-sub').textContent = sessionSubtitle(session);
    const badges = row.querySelector('.row-badges');
    R.clearNode(badges);
    for (const badge of sessionBadges(session)) badges.append(badge);
}

function sessionSubtitle(session) {
    const id = session.session_id;
    const loop = state.store.loop(id);
    const bits = [];
    if (loop?.status) bits.push(`loop ${loop.status}${loop.phase ? `/${loop.phase}` : ''}`);
    else if (session.last_event) bits.push(`last ${session.last_event}`);
    else bits.push('no events yet');
    const model = state.store.model(id);
    if (model) bits.push(model);
    return bits.join(' · ');
}

function renderSessionList() {
    const ids = state.store.ids();
    R.clearNode(ui.sessionList);
    if (ids.length === 0) {
        ui.sessionList.append(R.emptyState('No sessions',
            'Create one with "New session", then start its worker process.'));
        return;
    }
    const selected = state.store.selected();
    for (const id of ids) {
        const session = state.store.get(id);
        const [dotClass, dotTitle] = sessionDot(session);
        const row = R.el('button', {
            class: `session-row${id === selected ? ' selected' : ''}`,
            attrs: {
                type: 'button',
                'aria-current': id === selected ? 'true' : 'false',
                'data-session': id,
            },
            on: { click: () => selectSession(id) },
        }, [
            R.el('span', { class: 'row-top' }, [
                R.el('span', { class: `dot ${dotClass}`, title: dotTitle }),
                R.el('span', { class: 'row-id', text: id }),
            ]),
            R.el('span', { class: 'row-sub', text: sessionSubtitle(session) }),
            R.el('span', { class: 'row-badges' }, sessionBadges(session)),
        ]);
        ui.sessionList.append(row);
    }
}

// --------------------------------------------------------------- header ---

function renderHeader() {
    const sessionId = state.store.selected();
    const session = sessionId ? state.store.get(sessionId) : null;
    const actions = ['act-start', 'act-stop', 'act-restart', 'act-force-kill', 'act-status',
        'act-options', 'act-cancel', 'act-shutdown', 'act-delete'];
    for (const id of actions) {
        const button = ui[uiKey(id)];
        if (button) button.disabled = !session;
    }
    if (!session) {
        ui.sessionTitle.textContent = 'no session selected';
        R.clearNode(ui.sessionMeta);
        updateComposerHint();
        return;
    }
    ui.sessionTitle.textContent = session.session_id;
    const stats = state.store.stats(session.session_id);
    const loop = state.store.loop(session.session_id);
    const process = session.process;
    const identity = session.identity ?? {};
    const bits = [
        ['worker', R.shortId(identity.worker_id), identity.state ? `identity ${identity.state}` : ''],
        ['pid', process?.pid ? String(process.pid) : '—', process?.state ?? 'no process'],
        ['run', R.shortId(session.last_run_id ?? state.store.lastRunId(session.session_id))],
        ['loop', loop?.status ? `${loop.status}${loop.phase ? `/${loop.phase}` : ''}` : '—'],
        ['model', state.store.model(session.session_id) || '—'],
        ['events', String(stats.items)],
        ['last', session.last_event_at ? R.formatClock(session.last_event_at) : '—'],
    ];
    R.clearNode(ui.sessionMeta);
    for (const [label, value, title] of bits) {
        ui.sessionMeta.append(R.el('span', {
            class: 'mono',
            text: `${label} ${value}`,
            title: title || `${label} ${value}`,
        }));
    }
}

// ------------------------------------------------------------- timeline ---

function emptyTimeline() {
    const sessionId = state.store.selected();
    if (!sessionId) {
        return R.emptyState('No session selected', 'Choose a session on the left, or create one.');
    }
    const view = state.store.view(sessionId);
    if (view.items.length === 0) {
        return R.emptyState('No events yet',
            'The hub replays what it has; new envelopes appear here as they arrive.');
    }
    return null;
}

function renderTimelineFull() {
    R.clearNode(ui.timeline);
    state.requestElements = new Map();
    state.currentGroup = null;
    const placeholder = emptyTimeline();
    if (placeholder) {
        ui.timeline.append(placeholder);
        stickScroll(true);
        return;
    }
    const view = state.store.view(state.store.selected());
    if (view.droppedItems > 0) {
        ui.timeline.append(R.el('div', {
            class: 'note-line',
            text: `${view.droppedItems} older envelope(s) were dropped from this client view`
                + ' (2000-envelope cap); the hub transcript and its JSONL file still hold them',
        }));
    }
    for (const item of view.items) appendTimelineItem(item);
    stickScroll(true);
}

/** Append one transcript item, keeping run grouping. */
function appendTimelineItem(item) {
    if (!item || !state.store.selected()) return;
    if (item.kind === 'request') {
        const node = R.renderRequestChip(item.request);
        state.requestElements.set(item.request.request_id, node);
        appendToGroup(node);
        return;
    }
    if (item.kind === 'note') {
        appendToGroup(R.el('div', {
            class: `note-line${item.tone === 'warn' ? ' tone-warn' : ''}`,
            text: item.text,
        }));
        return;
    }
    const envelope = item.envelope;
    const name = envelope?.event;
    if (RUN_START_EVENTS.has(name)) {
        const group = R.el('section', { class: 'run-group' });
        group.append(R.el('div', {
            class: 'run-head',
            text: `run ${R.shortId(envelope.run_id)} · ${R.formatClock(envelope.received_at)}`,
        }));
        ui.timeline.append(group);
        state.currentGroup = group;
    }
    const node = R.renderEnvelope(envelope);
    appendToGroup(node);
    if (RUN_END_EVENTS.has(name)) state.currentGroup = null;
}

function appendToGroup(node) {
    if (state.currentGroup && state.currentGroup.isConnected) state.currentGroup.append(node);
    else ui.timeline.append(node);
}

function stickScroll(force = false) {
    const pane = ui.paneChat;
    const nearBottom = pane.scrollHeight - pane.scrollTop - pane.clientHeight < 80;
    if (force || nearBottom) pane.scrollTop = pane.scrollHeight;
}

// ----------------------------------------------------------- raw events ---

function isRawVisible() {
    return state.centerTab === 'raw';
}

function appendRawItem(item) {
    if (item.kind !== 'event') return;
    ui.rawList.append(R.renderRawEnvelope(item.envelope));
}

function renderRawFull() {
    R.clearNode(ui.rawList);
    state.rawDirty = false;
    const sessionId = state.store.selected();
    if (!sessionId) {
        ui.rawList.append(R.emptyState('No session selected', 'Raw envelopes appear here once a session is chosen.'));
        return;
    }
    const view = state.store.view(sessionId);
    const events = view.items.filter((item) => item.kind === 'event');
    if (events.length === 0) {
        ui.rawList.append(R.emptyState('No envelopes yet', 'Every envelope the hub forwards is kept here untouched.'));
        return;
    }
    for (const item of events) appendRawItem(item);
}

// ------------------------------------------------------------ composer ----

function partsFor(sessionId) {
    return state.parts.get(sessionId) ?? [];
}

function renderParts() {
    const sessionId = state.store.selected();
    R.clearNode(ui.partChips);
    if (!sessionId) return;
    const parts = partsFor(sessionId);
    parts.forEach((part, index) => {
        ui.partChips.append(R.el('span', { class: 'part-chip' }, [
            R.el('span', { class: 'part-text', text: `${part.type}: ${part.raw}` }),
            R.el('button', {
                text: '×',
                attrs: { type: 'button', 'aria-label': `Remove ${part.type} part` },
                on: {
                    click: () => {
                        parts.splice(index, 1);
                        state.parts.set(sessionId, parts);
                        renderParts();
                    },
                },
            }),
        ]));
    });
    const pending = state.pendingOptions.get(sessionId);
    if (pending && Object.keys(pending).length > 0) {
        const description = Object.entries(pending)
            .flatMap(([category, values]) => Object.entries(R.objSafe(values))
                .map(([key, value]) => `${category}.${key}=${value}`))
            .join(', ');
        ui.partChips.append(R.el('span', { class: 'part-chip' }, [
            R.el('span', { class: 'part-text', text: `next payload options: ${description}` }),
            R.el('button', {
                text: '×',
                attrs: { type: 'button', 'aria-label': 'Clear pending options' },
                on: {
                    click: () => {
                        state.pendingOptions.delete(sessionId);
                        renderParts();
                    },
                },
            }),
        ]));
    }
}

function addImageRef() {
    const sessionId = state.store.selected();
    if (!sessionId) {
        toast('warn', 'select a session first');
        return;
    }
    const value = ui.imageUrl.value.trim();
    if (!value) {
        toast('warn', 'enter an image URL first; it is sent as an external_ref content part');
        return;
    }
    const parts = partsFor(sessionId);
    parts.push({ type: 'external_ref', raw: value });
    state.parts.set(sessionId, parts);
    ui.imageUrl.value = '';
    renderParts();
    toast('info', 'external_ref part added — the model receives the reference, not the image bytes');
}

function buildOptions(sessionId) {
    const options = { confirmation: { mode: ui.confirmMode.value } };
    const pending = state.pendingOptions.get(sessionId);
    if (pending?.model && typeof pending.model === 'object'
        && Object.keys(pending.model).length > 0) {
        options.model = { ...pending.model };
    }
    return options;
}

function sendMessage() {
    const sessionId = state.store.selected();
    if (!sessionId) {
        toast('warn', 'select a session first');
        return;
    }
    const content = [];
    const text = ui.composerText.value.trim();
    if (text) content.push({ type: 'text', raw: text });
    for (const part of partsFor(sessionId)) content.push(part);
    if (content.length === 0) {
        toast('warn', 'nothing to send: write a message or add an image reference');
        return;
    }
    const sent = state.socket?.send({
        type: 'input',
        session: sessionId,
        content,
        options: buildOptions(sessionId),
    });
    if (!sent) {
        toast('error', 'the panel socket is not connected; nothing was sent');
        return;
    }
    ui.composerText.value = '';
    state.parts.set(sessionId, []);
    renderParts();
    updateComposerHint();
}

function sendContinue() {
    const sessionId = state.store.selected();
    if (!sessionId) {
        toast('warn', 'select a session first');
        return;
    }
    const sent = state.socket?.send({
        type: 'input',
        session: sessionId,
        operation: 'continue',
        options: buildOptions(sessionId),
    });
    if (!sent) {
        toast('error', 'the panel socket is not connected; nothing was sent');
        return;
    }
    updateComposerHint();
}

function updateComposerHint() {
    const sessionId = state.store.selected();
    const enabled = Boolean(sessionId);
    ui.send.disabled = !enabled;
    ui.continue.disabled = !enabled;
    ui.addImage.disabled = !enabled;
    ui.confirmMode.disabled = !enabled;
    ui.composerText.disabled = !enabled;
    ui.composerHint.className = 'composer-hint';
    if (!sessionId) {
        ui.composerHint.textContent = 'select a session to enable the composer';
        return;
    }
    if (state.store.isRunActive(sessionId)) {
        ui.composerHint.className = 'composer-hint hint-warn';
        ui.composerHint.textContent = 'run active: the payload is queued until the current run settles';
        return;
    }
    ui.composerHint.textContent = 'no delivery acknowledgement: sent is not executed';
}

// ------------------------------------------------------------ inspector ---

function renderInspector() {
    for (const tab of ui.inspectorTabs) {
        const active = tab.dataset.itab === state.inspectorTab;
        tab.classList.toggle('active', active);
        tab.setAttribute('aria-selected', active ? 'true' : 'false');
        const pane = ui.inspectorPanes[tab.dataset.itab];
        pane.hidden = !active;
    }
    switch (state.inspectorTab) {
        case 'status': renderStatusPane(); return;
        case 'options': renderOptionsPane(); return;
        case 'process': renderProcessPane(); return;
        case 'logs': renderLogsPane(); return;
        case 'snapshot': renderSnapshotPane(); return;
        default:
    }
}

function currentSession() {
    const id = state.store.selected();
    return id ? state.store.get(id) : null;
}

function renderStatusPane() {
    const pane = ui.inspectorPanes.status;
    R.clearNode(pane);
    const session = currentSession();
    if (!session) {
        pane.append(R.emptyState('No session selected', 'Session state appears here.'));
        return;
    }
    const id = session.session_id;
    const stats = state.store.stats(id);
    pane.append(R.el('div', { class: 'kv-title', text: 'session' }));
    pane.append(R.kv([
        ['created', session.created_at ? R.formatStamp(session.created_at) : ''],
        ['connected', session.connected ? 'true' : 'false'],
        ['identity', `${session.identity?.state ?? 'unknown'} · ${R.shortId(session.identity?.worker_id)}`],
        ['last_event', session.last_event ?? ''],
        ['last_event_at', session.last_event_at ? R.formatStamp(session.last_event_at) : ''],
        ['last_run_id', session.last_run_id ?? ''],
        ['provider', session.spec?.provider ?? ''],
        ['model', state.store.model(id)],
        ['threads', session.spec?.threads],
        ['maxExchanges', session.spec?.maxExchanges],
        ['restore', session.spec?.restore],
        ['persistence', session.spec?.persistence
            ? `enabled=${session.spec.persistence.enabled === true}, readable=${session.spec.persistence.readable === true}`
            : ''],
        ['workspace', session.spec?.workspace ?? ''],
    ]));
    const hubStats = session.stats ?? {};
    pane.append(R.el('div', { class: 'kv-title', text: 'hub counters' }));
    pane.append(R.kv([
        ['events', hubStats.events ?? 0],
        ['gaps', hubStats.gaps ?? 0],
        ['duplicates', hubStats.duplicates ?? 0],
        ['protocolErrors', hubStats.protocolErrors ?? 0],
        ['incarnations', hubStats.incarnations ?? 0],
        ['client view', `${stats.items} envelope(s), hub_sequence ${stats.lastSeq}`],
        ['client duplicates dropped', stats.duplicates],
    ]));
    pane.append(R.el('div', { class: 'kv-title', text: 'latest status snapshot' }));
    const data = state.store.statusData(id);
    if (data) pane.append(R.statusBody(data));
    else {
        pane.append(R.emptyState('No status snapshot yet',
            'Use the Status button to ask the worker for one; nothing is fetched automatically.'));
    }
}

function renderOptionsPane() {
    const pane = ui.inspectorPanes.options;
    R.clearNode(pane);
    const session = currentSession();
    if (!session) {
        pane.append(R.emptyState('No session selected', 'Worker options appear here.'));
        return;
    }
    const id = session.session_id;
    const data = state.store.latestData(id, 'options');
    const panel = R.renderOptionsPanel(data);
    pane.append(panel);
    const apply = R.el('button', {
        class: 'btn btn-sm',
        text: 'Apply at next run',
        attrs: { type: 'button' },
        on: {
            click: () => {
                const selections = R.readOptionSelections(panel);
                const model = selections.model ?? {};
                const next = {};
                if (Object.keys(model).length > 0) next.model = model;
                const mode = selections.confirmation?.mode;
                if (typeof mode === 'string' && mode) ui.confirmMode.value = mode;
                state.pendingOptions.set(id, next);
                renderParts();
                toast('info', 'selections attached to the next payload as data.options'
                    + ' — they take effect at the next run boundary, not now');
            },
        },
    });
    const clear = R.el('button', {
        class: 'btn btn-sm btn-ghost',
        text: 'Clear',
        attrs: { type: 'button' },
        on: {
            click: () => {
                state.pendingOptions.delete(id);
                renderParts();
            },
        },
    });
    pane.append(R.el('div', { class: 'ipane-head' }, [
        R.el('span', { class: 'opt-note', text: 'these are payload options, never a signal' }),
        apply,
        clear,
    ]));
    if (!data) {
        pane.append(R.el('div', {
            class: 'opt-note',
            text: 'No options snapshot in this transcript yet. Use the Options button'
                + ' in the header to ask the worker for its current choices.',
        }));
    }
}

function renderProcessPane() {
    const pane = ui.inspectorPanes.process;
    R.clearNode(pane);
    const session = currentSession();
    if (!session) {
        pane.append(R.emptyState('No session selected', 'Process state appears here.'));
        return;
    }
    pane.append(R.renderProcessPanel(session.process));
}

function renderLogsPane() {
    const pane = ui.inspectorPanes.logs;
    R.clearNode(pane);
    const session = currentSession();
    if (!session) {
        pane.append(R.emptyState('No session selected', 'Worker output appears here.'));
        return;
    }
    const id = session.session_id;
    pane.append(R.el('div', { class: 'ipane-head' }, [
        R.el('span', { class: 'kv-title', text: 'worker stdout/stderr tail' }),
        R.el('button', {
            class: 'btn btn-sm',
            text: 'Refresh',
            attrs: { type: 'button' },
            on: {
                click: () => {
                    const sent = state.socket?.send({
                        type: 'logs', session: id, limit: LOG_REQUEST_LIMIT,
                    });
                    if (!sent) toast('warn', 'the panel socket is not connected');
                },
            },
        }),
    ]));
    pane.append(R.renderLogsPanel(state.store.logs(id)));
}

async function renderSnapshotPane() {
    const pane = ui.inspectorPanes.snapshot;
    R.clearNode(pane);
    const session = currentSession();
    if (!session) {
        pane.append(R.emptyState('No session selected', 'The persistence snapshot appears here.'));
        return;
    }
    const id = session.session_id;
    pane.append(R.el('div', { class: 'ipane-head' }, [
        R.el('span', { class: 'kv-title', text: 'persistence snapshot (read-only)' }),
        R.el('button', {
            class: 'btn btn-sm',
            text: 'Load snapshot',
            attrs: { type: 'button' },
            on: {
                click: async () => {
                    try {
                        const snapshot = await state.rest.snapshot(id);
                        state.snapshots.set(id, snapshot);
                    } catch (error) {
                        toast('error', `snapshot unavailable: ${error.message}`);
                        state.snapshots.set(id, { session_id: id, state: null, readable: null, files: {} });
                    }
                    if (state.inspectorTab === 'snapshot') renderSnapshotPane();
                },
            },
        }),
    ]));
    pane.append(R.renderSnapshotPanel(state.snapshots.get(id) ?? null));
}

// -------------------------------------------------------- confirmations ---

function openConfirmationModal(prompt) {
    if (!prompt?.confirmation_id) return;
    const id = `confirm:${prompt.confirmation_id}`;
    const existing = state.modals.find((modal) => modal.id === id);
    if (existing && existing.stateKey === prompt.state) return;
    hiddenConfirmations.delete(prompt.confirmation_id);
    const element = R.renderConfirmationModal(prompt, {
        onDecide: (decision, reason) => decideConfirmation(prompt, decision, reason),
        onMinimize: () => {
            hiddenConfirmations.add(prompt.confirmation_id);
            closeModal(id);
            updateConfirmBanner();
        },
    });
    const entry = openModal(id, element, {
        stateKey: prompt.state,
        onEscape: () => {
            hiddenConfirmations.add(prompt.confirmation_id);
            closeModal(id);
            updateConfirmBanner();
        },
    });
    entry.promptData = prompt;
    updateConfirmBanner();
}

function closeConfirmationModal(change) {
    const confirmationId = change.prompt?.confirmation_id;
    if (!confirmationId) return;
    hiddenConfirmations.delete(confirmationId);
    closeModal(`confirm:${confirmationId}`);
    const phase = change.outcome?.phase ?? 'unknown';
    const detail = change.outcome?.detail ? ` (${change.outcome.detail})` : '';
    const decision = change.prompt?.decision ? ` · decision ${change.prompt.decision}` : '';
    toast(phase === 'decided' ? 'info' : 'warn',
        `confirmation for ${change.prompt?.call?.name ?? 'call'} closed: ${OUTCOME_TEXT[phase] ?? phase}${detail}${decision}`);
    updateConfirmBanner();
}

function decideConfirmation(prompt, decision, reason) {
    const sent = state.socket?.send({
        type: 'confirmation',
        session: prompt.session_id,
        confirmation_id: prompt.confirmation_id,
        decision,
        reason,
    });
    if (!sent) {
        toast('error', 'the panel socket is not connected; no decision was sent');
        return;
    }
    toast('info', `decision "${decision}" sent — the hub must still settle the prompt,`
        + ' and an approval does not prove the call ran');
}

function updateConfirmBanner() {
    const open = [];
    for (const id of state.store.ids()) {
        for (const prompt of state.store.openConfirmations(id)) open.push(prompt);
    }
    const hidden = open.filter((prompt) => hiddenConfirmations.has(prompt.confirmation_id));
    if (hidden.length === 0) {
        ui.confirmBanner.hidden = true;
        R.clearNode(ui.confirmBanner);
        return;
    }
    ui.confirmBanner.hidden = false;
    R.clearNode(ui.confirmBanner);
    ui.confirmBanner.append(R.el('span', {
        text: `${hidden.length} confirmation prompt(s) hidden`,
    }));
    ui.confirmBanner.append(R.el('button', {
        class: 'btn btn-sm',
        text: 'Review',
        attrs: { type: 'button' },
        on: {
            click: () => {
                for (const prompt of hidden) hiddenConfirmations.delete(prompt.confirmation_id);
                for (const prompt of hidden) openConfirmationModal(prompt);
                updateConfirmBanner();
            },
        },
    }));
}

function restoreOpenConfirmations() {
    for (const id of state.store.ids()) {
        for (const prompt of state.store.openConfirmations(id)) {
            if (!modalOpen(`confirm:${prompt.confirmation_id}`)) openConfirmationModal(prompt);
        }
    }
    updateConfirmBanner();
}

// ------------------------------------------------------------- selection --

function renderSelection() {
    renderSessionList();
    renderHeader();
    renderTimelineFull();
    renderRawFull();
    renderParts();
    updateComposerHint();
    renderInspector();
    restoreOpenConfirmations();
}

// -------------------------------------------------------------- actions ---

function sendSignal(operation) {
    const sessionId = state.store.selected();
    if (!sessionId) return;
    const message = { type: 'signal', session: sessionId, operation };
    if (operation === 'cancel') {
        const runId = state.store.lastRunId(sessionId);
        if (runId) message.run_id = runId;
    }
    if (!state.socket?.send(message)) {
        toast('error', 'the panel socket is not connected; no signal was sent');
    }
}

function sendWorkerAction(action) {
    const sessionId = state.store.selected();
    if (!sessionId) return;
    if (action === 'force-kill') {
        const session = state.store.get(sessionId);
        const pid = session?.process?.pid ?? 'unknown';
        const confirmed = globalThis.confirm(
            `Force-kill the worker process for "${sessionId}" (pid ${pid})?\n\n`
            + 'This sends SIGKILL and skips the protocol shutdown handshake. It can kill the'
            + ' process group, including descendants the worker would not have terminated.'
            + ' The worker cannot persist final state after SIGKILL.');
        if (!confirmed) return;
    }
    if (!state.socket?.send({ type: 'worker', session: sessionId, action })) {
        toast('error', 'the panel socket is not connected; no action was sent');
    }
}

function deleteSession() {
    const sessionId = state.store.selected();
    if (!sessionId) return;
    const session = state.store.get(sessionId);
    if (session?.connected || ['starting', 'running', 'stopping'].includes(session?.process?.state)) {
        toast('warn', 'stop the worker and let it disconnect before deleting the session');
        return;
    }
    if (!globalThis.confirm(`Delete session "${sessionId}"? Its hub transcript is dropped;`
        + ' the worker-owned persistence directory on disk is left alone.')) return;
    if (!state.socket?.send({ type: 'delete_session', session: sessionId })) {
        toast('error', 'the panel socket is not connected; nothing was deleted');
    }
}

function openNewSessionModal() {
    const form = R.renderNewSessionForm({
        hub: state.store.hub(),
        onSubmit: ({ session, spec }) => {
            const sent = state.socket?.send({ type: 'create_session', session, spec });
            if (!sent) {
                form.setError('the panel socket is not connected; the session was not created');
                return;
            }
            state.createPending = form;
        },
    });
    openModal('new-session', R.el('div', { class: 'modal modal-narrow' }, [
        R.el('div', { class: 'modal-head' }, [
            R.el('span', { class: 'modal-title', text: 'new session' }),
            R.el('button', {
                class: 'btn btn-ghost btn-sm',
                text: 'close',
                attrs: { type: 'button', 'aria-label': 'Close the new session dialog' },
                on: { click: () => { state.createPending = null; closeModal('new-session'); } },
            }),
        ]),
        form.element,
    ]), {
        onEscape: () => {
            state.createPending = null;
            closeModal('new-session');
        },
    });
    form.sessionInput.focus();
}

// --------------------------------------------------------------- wiring ---

function wireTabs() {
    ui.tabChat.addEventListener('click', () => {
        state.centerTab = 'chat';
        ui.tabChat.classList.add('active');
        ui.tabRaw.classList.remove('active');
        ui.tabChat.setAttribute('aria-selected', 'true');
        ui.tabRaw.setAttribute('aria-selected', 'false');
        ui.paneChat.hidden = false;
        ui.paneRaw.hidden = true;
        stickScroll(true);
    });
    ui.tabRaw.addEventListener('click', () => {
        state.centerTab = 'raw';
        ui.tabRaw.classList.add('active');
        ui.tabChat.classList.remove('active');
        ui.tabRaw.setAttribute('aria-selected', 'true');
        ui.tabChat.setAttribute('aria-selected', 'false');
        ui.paneRaw.hidden = false;
        ui.paneChat.hidden = true;
        if (state.rawDirty) renderRawFull();
    });
}

function wireInspector() {
    for (const tab of ui.inspectorTabs) {
        tab.addEventListener('click', () => {
            state.inspectorTab = tab.dataset.itab;
            renderInspector();
        });
    }
    ui.inspectorToggle.addEventListener('click', () => {
        const collapsed = ui.app.classList.toggle('inspector-collapsed');
        ui.inspectorToggle.setAttribute('aria-expanded', collapsed ? 'false' : 'true');
    });
}

function wireComposer() {
    ui.send.addEventListener('click', sendMessage);
    ui.continue.addEventListener('click', sendContinue);
    ui.addImage.addEventListener('click', addImageRef);
    ui.composerText.addEventListener('keydown', (event) => {
        if (event.key === 'Enter' && !event.shiftKey) {
            event.preventDefault();
            sendMessage();
        }
    });
}

function wireActions() {
    ui.newSession.addEventListener('click', openNewSessionModal);
    ui.refreshSessions.addEventListener('click', () => {
        state.socket?.send({ type: 'list_sessions' });
        state.rest?.sessions()
            .then((result) => state.store.setSessions(result.sessions ?? []))
            .catch(() => { /* the panel socket is the primary source */ });
    });
    ui.actStart.addEventListener('click', () => sendWorkerAction('start'));
    ui.actStop.addEventListener('click', () => sendWorkerAction('stop'));
    ui.actRestart.addEventListener('click', () => sendWorkerAction('restart'));
    ui.actForceKill.addEventListener('click', () => sendWorkerAction('force-kill'));
    ui.actStatus.addEventListener('click', () => sendSignal('status'));
    ui.actOptions.addEventListener('click', () => sendSignal('options'));
    ui.actCancel.addEventListener('click', () => sendSignal('cancel'));
    ui.actShutdown.addEventListener('click', () => sendSignal('shutdown'));
    ui.actDelete.addEventListener('click', deleteSession);
    ui.themeToggle.addEventListener('click', () => {
        applyTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark');
    });
    ui.tokenForm.addEventListener('submit', submitToken);
    document.addEventListener('keydown', onDocumentKeydown);
}

/** Cache every element the app touches. */
function collectElements() {
    const ids = [
        'app', 'sidebar', 'hub-name', 'hub-version', 'panel-badge', 'theme-toggle',
        'new-session', 'refresh-sessions', 'session-list', 'center', 'session-title',
        'session-meta', 'inspector-toggle', 'confirm-banner', 'tab-chat', 'tab-raw',
        'pane-chat', 'pane-raw', 'timeline', 'raw-list', 'part-chips', 'composer-text',
        'send', 'continue', 'image-url', 'add-image', 'confirm-mode', 'composer-hint',
        'inspector', 'inspector-body', 'modal-root', 'modal-stack', 'toast-root',
        'token-overlay', 'token-form', 'token-input', 'token-error', 'token-submit',
        'act-start', 'act-stop', 'act-restart', 'act-force-kill', 'act-status',
        'act-options', 'act-cancel', 'act-shutdown', 'act-delete',
        'ipane-status', 'ipane-options', 'ipane-process', 'ipane-logs', 'ipane-snapshot',
    ];
    for (const id of ids) {
        const node = $(id);
        if (!node) {
            globalThis.console?.error?.(`panel: element #${id} is missing from index.html`);
            continue;
        }
        ui[uiKey(id)] = node;
    }
    ui.inspectorTabs = [...document.querySelectorAll('[data-itab]')];
    ui.inspectorPanes = {
        status: $('ipane-status'),
        options: $('ipane-options'),
        process: $('ipane-process'),
        logs: $('ipane-logs'),
        snapshot: $('ipane-snapshot'),
    };
}

/** Entry point: called once by main.js. */
export function init() {
    if (typeof document === 'undefined') return null;
    collectElements();
    applyTheme(initialTheme());

    state.tokens = createTokenStore();
    state.store = createStore();
    state.store.subscribe(onStoreChange);

    wireTabs();
    wireInspector();
    wireComposer();
    wireActions();
    startCountdownTicker();
    renderHub();
    renderPanelBadge();
    renderSelection();
    globalThis.__panelInternals = internals;
    void bootstrap();
    return state;
}

/**
 * Debug handle for an operator with the console open. It deliberately exposes
 * no token material, and the panel itself never reads it.
 */
export const internals = {
    ui,
    R,
    get store() { return state.store; },
    get socket() { return state.socket; },
    get rest() { return state.rest; },
};
