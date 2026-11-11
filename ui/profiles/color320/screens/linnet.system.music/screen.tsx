import { Screen, StatusBar, VirtualList, Scrollbar, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Music" bg="theme.panel" deps={16} />
  <VirtualList x={5} y={32} w={293} h={169} count={50000} text="Track " focus={true} />
  <Scrollbar x={305} y={32} w={8} h={169} count={50000} fg="theme.accent" />
  <Text x={5} y={208} w={308} h={27} text="Right: Actions  Menu: Back" fg="theme.muted" />
</Screen>
  );
}
