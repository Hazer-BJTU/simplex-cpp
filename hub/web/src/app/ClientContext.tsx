/**
 * @file the client, handed down through React.
 *
 * One client per page, and components need it to send anything. A context
 * rather than a module singleton so a test can mount the tree against a stub
 * client without stubbing the network — and so the entry point stays the only
 * place that decides which hub this page is talking to.
 */
import { createContext, useContext, type ReactNode } from 'react';
import type { PanelClient } from '../lib/client.ts';

const ClientContext = createContext<PanelClient | null>(null);

export function ClientProvider({ client, children }: {
    client: PanelClient;
    children: ReactNode;
}) {
    return <ClientContext.Provider value={client}>{children}</ClientContext.Provider>;
}

/** The client for this page. Throws rather than returning null: a component
 *  rendered outside the provider is a wiring mistake, not a runtime state. */
export function useClient(): PanelClient {
    const client = useContext(ClientContext);
    if (!client) throw new Error('useClient was called outside <ClientProvider>');
    return client;
}
