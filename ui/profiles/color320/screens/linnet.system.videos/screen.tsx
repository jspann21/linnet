import { Screen, StatusBar, Scrollbar, VirtualList } from "@linnet/ui";
export default function Scene() { return (
<Screen><StatusBar w={320} h={28} text="Videos" bg="theme.panel" />
<VirtualList x={12} y={36} w={284} h={196} count={0} text="Videos" focus={true} />
  <Scrollbar x={306} y={36} w={4} h={196} fg="theme.accent" />
</Screen>
); }
