/**
 * @file panel entry point.
 *
 * The only module that touches the DOM at import time; everything else is
 * declarations and components. It is also the only place the client is created,
 * which keeps "which hub is this page talking to" a single decision: same
 * origin, a `?token=` if the URL carried one, and `vite dev`'s proxy when the
 * page is being iterated rather than served.
 */
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './app/App.tsx';
import { ClientProvider } from './app/ClientContext.tsx';
import { createPanelClient } from './lib/client.ts';
import './styles/app.css';

const container = document.getElementById('root');
if (!container) throw new Error('app.html has no #root element');

const client = createPanelClient();

createRoot(container).render(
    <StrictMode>
        <ClientProvider client={client}>
            <App />
        </ClientProvider>
    </StrictMode>,
);

// Started after the first render so the shell paints before any request
// settles: a hub that is not running should show a panel that says so, not a
// blank page.
void client.start();
