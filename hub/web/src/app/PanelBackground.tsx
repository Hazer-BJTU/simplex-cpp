/** One page-owned image, isolated from session/transcript rendering. */
import { useEffect, useState, type CSSProperties } from 'react';
import { usePanel } from '../state/usePanel.ts';
import { useClient } from './ClientContext.tsx';
import { normalizeBackground } from '../../../shared/background.ts';

/**
 * Fetch with bearer authentication, decode before revealing surfaces, and revoke
 * every object URL on replacement/unmount. Theme changes use CSS only. Stable
 * primitive dependencies prevent metadata repeats from restarting the request.
 */
export function PanelBackground() {
    const client = useClient();
    const descriptor = usePanel(state => state.hub?.background);
    const epoch = usePanel(state => state.hub?.transcript_epoch);
    const connected = usePanel(state => state.connection.state === 'open');
    const unauthorized = usePanel(state => state.authRequired);
    const key = descriptor ? JSON.stringify(descriptor) : '';
    const [image, setImage] = useState<{ key: string; epoch: string | undefined; url: string; size: string; position: string } | null>(null);

    useEffect(() => {
        setImage(null);
        if (!connected || unauthorized || !key) return;
        let options;
        try {
            const metadata = JSON.parse(key);
            if (metadata.url !== '/api/panel/background/image' || !/^[a-f0-9]{64}$/.test(metadata.revision)) return;
            options = normalizeBackground({ image: '', size: metadata.size, position: metadata.position,
                opacity: metadata.opacity, toolCardOpacity: metadata.toolCardOpacity });
        } catch { return; }
        if (!options.opacity.light && !options.opacity.dark) return;
        const controller = new AbortController();
        let disposed = false;
        let objectUrl: string | undefined;
        const root = document.documentElement;
        const settings = options;
        void client.rest.background(controller.signal).then(async blob => {
            if (disposed) return;
            objectUrl = URL.createObjectURL(blob);
            const decoded = new Image();
            decoded.src = objectUrl;
            await decoded.decode();
            if (disposed) return;
            root.style.setProperty('--panel-image-light-opacity', String(settings.opacity.light));
            root.style.setProperty('--panel-image-dark-opacity', String(settings.opacity.dark));
            root.style.setProperty('--panel-tool-card-light-opacity', `${settings.toolCardOpacity.light * 100}%`);
            root.style.setProperty('--panel-tool-card-dark-opacity', `${settings.toolCardOpacity.dark * 100}%`);
            root.dataset.panelBackground = [settings.opacity.light > 0 ? 'light' : '',
                settings.opacity.dark > 0 ? 'dark' : ''].filter(Boolean).join(' ');
            setImage({ key, epoch, url: objectUrl, size: settings.size, position: settings.position });
        }).catch(() => {
            // Decoration failure does not enter the conversation/error stream.
            if (objectUrl) { URL.revokeObjectURL(objectUrl); objectUrl = undefined; }
        });
        return () => {
            disposed = true;
            controller.abort();
            if (objectUrl) URL.revokeObjectURL(objectUrl);
            delete root.dataset.panelBackground;
            root.style.removeProperty('--panel-image-light-opacity');
            root.style.removeProperty('--panel-image-dark-opacity');
            root.style.removeProperty('--panel-tool-card-light-opacity');
            root.style.removeProperty('--panel-tool-card-dark-opacity');
        };
    }, [client, key, epoch, connected, unauthorized]);

    const current = connected && !unauthorized && image?.key === key && image.epoch === epoch ? image : null;
    return <div aria-hidden="true" data-testid="panel-background" className="panel-background"
        style={current ? { backgroundImage: `url("${current.url}")`, backgroundSize: current.size,
            backgroundPosition: current.position } as CSSProperties : undefined} />;
}
