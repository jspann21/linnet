import { Screen, StatusBar, VirtualList, Scrollbar, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Play Queue" bg="theme.panel" deps={16} />
  <VirtualList x={5} y={32} w={293} h={169} count={1} text="Track " focus={true} />
  <Scrollbar x={305} y={32} w={8} h={169} count={1} fg="theme.accent" />
  <Text x={5} y={208} w={308} h={27} text="Select: Actions  Menu: Back" fg="theme.muted" />
</Screen>
  );
}
