import { memo, useEffect, useState } from 'react';
import * as TabsPrimitive from '@radix-ui/react-tabs';
import { usePanel } from '../state/usePanel.ts';
import { Tabs, TabsList, TabsTrigger } from '../ui/overlays.tsx';
import { Approvals } from './Approvals.tsx';
import { DecisionRegion } from './DecisionRegion.tsx';
import { Markdown } from './Markdown.tsx';
import { OperatorQuestions } from './OperatorQuestions.tsx';
import { Transcript } from './Transcript.tsx';

/** Reset the selected pane when switching sessions; incoming plans never steal focus. */
export function ConversationPanels() {
    const selected = usePanel((state) => state.selected);
    const markdown = usePanel((state) => selected ? state.plans.get(selected)?.markdown ?? '' : '');
    const [paneSession, setPaneSession] = useState(selected);
    const [pane, setPane] = useState('conversation');
    const hasPlan = markdown.trim().length > 0;
    const activePane = hasPlan ? pane : 'conversation';

    if (paneSession !== selected) {
        setPaneSession(selected);
        setPane('conversation');
    }

    useEffect(() => {
        if (!hasPlan) setPane('conversation');
    }, [hasPlan]);

    return (
        <Tabs value={activePane} onValueChange={setPane} className="flex min-h-0 flex-1 flex-col">
            {hasPlan && (
                <TabsList label="conversation panels" bordered={false}>
                    <TabsTrigger id="conversation-tab" value="conversation">Conversation</TabsTrigger>
                    <TabsTrigger id="plan-tab" value="plan">Plan</TabsTrigger>
                </TabsList>
            )}
            <div className="relative flex min-h-0 flex-1 flex-col">
                {/* Keep both panes mounted while switching tabs. Only the
                    transcript and plan belong to an individual session. */}
                <div
                    role={hasPlan ? 'tabpanel' : undefined}
                    aria-labelledby={hasPlan ? 'conversation-tab' : undefined}
                    style={{ display: activePane === 'conversation' ? 'flex' : 'none' }}
                    className="min-h-0 flex-1 flex-col focus:outline-none"
                    data-decision-surface
                >
                    <Transcript key={selected} active={activePane === 'conversation'} />
                </div>
                {hasPlan && (
                    <TabsPrimitive.Content
                        key={selected}
                        value="plan"
                        forceMount
                        aria-labelledby="plan-tab"
                        style={{ display: activePane === 'plan' ? 'block' : 'none' }}
                        className="min-h-0 flex-1 overflow-auto break-words reading-scroll text-sm focus:outline-none"
                        data-testid="plan-content"
                        data-decision-surface
                        data-decision-fallback
                    >
                        <PlanContent active={activePane === 'plan'} markdown={markdown} />
                    </TabsPrimitive.Content>
                )}
                <DecisionRegion>
                    <Approvals />
                    <OperatorQuestions />
                </DecisionRegion>
            </div>
        </Tabs>
    );
}

/** Preserve rendered plan/disclosure state while hidden; refresh on return. */
const PlanContent = memo(function PlanContent({ markdown }: { markdown: string; active: boolean }) {
    return <div className="reading-width"><Markdown>{markdown}</Markdown></div>;
}, (previous, next) => !next.active || previous.markdown === next.markdown);
