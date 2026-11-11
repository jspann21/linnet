import { Screen, StatusBar, VirtualList, Scrollbar, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Sleep Timer" bg="theme.panel" deps={16} />
  <VirtualList x={5} y={32} w={293} h={147} count={4} text="Sleep timer" items={['After: 15 minutes','Fade out: Off','Start timer','Cancel timer']} focus={true} />
  <Scrollbar x={305} y={32} w={8} h={147} count={4} fg="theme.accent" />
  <Text x={5} y={196} w={308} h={35} text="Sleep timer status" fg="theme.muted" />
</Screen>
  );
}
