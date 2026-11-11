import { Screen, StatusBar, VirtualList, Scrollbar, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Themes" bg="theme.panel" deps={16} />
  <VirtualList x={4} y={24} w={202} h={124} text="Theme choices" count={7} focus={true} />
  <Scrollbar x={210} y={24} w={6} h={124} text="Theme choices" count={7} fg="theme.accent" />
  <Text x={4} y={153} w={212} h={20} text="Theme status" fg="theme.muted" />
</Screen>
  );
}
