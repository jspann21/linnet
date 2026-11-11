import { Screen, StatusBar, Scrollbar, VirtualList } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Photo Options" bg="theme.panel" deps={16} />
  <VirtualList x={4} y={24} w={202} h={148} count={7} text="Photo controls" focus={true} />
  <Scrollbar x={210} y={24} w={6} h={148} fg="theme.accent" />
</Screen>
  );
}
