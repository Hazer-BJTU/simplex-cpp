import { memo, useEffect, useState } from 'react';
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
                    <TabsTrigger id="conversation-tab" value="conversation">Conversation</TabsTrigger>
                    <TabsTrigger id="plan-tab" value="plan">Plan</TabsTrigger>
                </TabsList>
            )}
            {/* Keep the transcript mounted, including its scroll state. Without a
                plan it is ordinary layout, so no orphan tabpanel is exposed. */}
            <div
                role={hasPlan ? 'tabpanel' : undefined}
                aria-labelledby={hasPlan ? 'conversation-tab' : undefined}
                style={{ display: activePane === 'conversation' ? 'flex' : 'none' }}
                className="min-h-0 flex-1 flex-col focus:outline-none"
            >
                <Transcript active={activePane === 'conversation'} />
            </div>
            {hasPlan && (
                <TabsPrimitive.Content
                    value="plan"
                    forceMount
                    aria-labelledby="plan-tab"
                    style={{ display: activePane === 'plan' ? 'block' : 'none' }}
                    className="min-h-0 flex-1 overflow-auto break-words reading-scroll text-sm focus:outline-none"
                    data-testid="plan-content"
                >
                    <PlanContent active={activePane === 'plan'} markdown={markdown} />
                </TabsPrimitive.Content>
            )}
        </Tabs>
    );
}

/** Preserve rendered plan/disclosure state while hidden; refresh on return. */
const PlanContent = memo(function PlanContent({ markdown }: { markdown: string; active: boolean }) {
    return <div className="reading-width"><Markdown>{markdown}</Markdown></div>;
}, (previous, next) => !next.active || previous.markdown === next.markdown);
