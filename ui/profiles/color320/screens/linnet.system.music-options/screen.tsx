import { Screen, StatusBar, VirtualList, Scrollbar } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Playback Options" bg="theme.panel" deps={16} />
  <VirtualList x={12} y={36} w={284} h={196} count={13} text="Option " focus={true} />
  <Scrollbar x={306} y={36} w={4} h={196} count={13} fg="theme.accent" />
</Screen>
  );
}
