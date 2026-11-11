import { Screen, StatusBar, VirtualList, Scrollbar, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Sleep Timer" bg="theme.panel" deps={16} />
  <VirtualList x={4} y={24} w={202} h={108} count={4} text="Sleep timer" items={['After: 15 minutes','Fade out: Off','Start timer','Cancel timer']} focus={true} />
  <Scrollbar x={210} y={24} w={6} h={108} count={4} fg="theme.accent" />
  <Text x={4} y={144} w={212} h={26} text="Sleep timer status" fg="theme.muted" />
</Screen>
  );
}
