/**
 * @file panel entry point.
 *
 * The only module that touches the DOM at import time; everything else is
 * declarations and components, so each file stays loadable outside a browser.
 */
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './app/App.tsx';
import './styles/app.css';

const container = document.getElementById('root');
if (!container) throw new Error('index.html has no #root element');

createRoot(container).render(
    <StrictMode>
        <App />
    </StrictMode>,
);
