import { useEffect, useState } from 'react';
import * as TabsPrimitive from '@radix-ui/react-tabs';
import { usePanel } from '../state/usePanel.ts';
import { Tabs, TabsList, TabsTrigger } from '../ui/overlays.tsx';
import { Markdown } from './Markdown.tsx';
import { Transcript } from './Transcript.tsx';

/** Reset the selected pane when switching sessions; incoming plans never steal focus. */
export function ConversationPanels() {
    const selected = usePanel((state) => state.selected);
    const markdown = usePanel((state) => selected ? state.plans.get(selected)?.markdown ?? '' : '');
    return <SessionPanels key={selected} markdown={markdown} />;
}

/** Plan and conversation share one scrollable area while the composer stays in place. */
function SessionPanels({ markdown }: { markdown: string }) {
    const [pane, setPane] = useState('conversation');
    const hasPlan = markdown.trim().length > 0;
    const activePane = hasPlan ? pane : 'conversation';

    useEffect(() => {
        if (!hasPlan) setPane('conversation');
    }, [hasPlan]);

    return (
        <Tabs value={activePane} onValueChange={setPane} className="flex min-h-0 flex-1 flex-col">
            {hasPlan && (
                <TabsList label="conversation panels">
                    <TabsTrigger value="conversation">Conversation</TabsTrigger>
                    <TabsTrigger value="plan">Plan</TabsTrigger>
                </TabsList>
            )}
            {/* Keep transcript state and scroll position across tab switches. Explicit
                display:none overrides the flex layout of an inactive mounted pane. */}
            <TabsPrimitive.Content
                value="conversation"
                forceMount
                style={{ display: activePane === 'conversation' ? 'flex' : 'none' }}
                className="min-h-0 flex-1 flex-col focus:outline-none"
            >
                <Transcript />
            </TabsPrimitive.Content>
            {hasPlan && (
                <TabsPrimitive.Content
                    value="plan"
                    forceMount
                    style={{ display: activePane === 'plan' ? 'block' : 'none' }}
                    className="min-h-0 flex-1 overflow-auto break-words p-4 text-sm focus:outline-none"
                    data-testid="plan-content"
                >
                    <Markdown>{markdown}</Markdown>
                </TabsPrimitive.Content>
            )}
        </Tabs>
    );
}
