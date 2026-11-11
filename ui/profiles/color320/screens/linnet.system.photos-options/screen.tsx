import { Screen, StatusBar, Scrollbar, VirtualList } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Photo Options" bg="theme.panel" deps={16} />
  <VirtualList x={12} y={36} w={284} h={196} count={7} text="Photo controls" focus={true} />
  <Scrollbar x={306} y={36} w={4} h={196} fg="theme.accent" />
</Screen>
  );
}
