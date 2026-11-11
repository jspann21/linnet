import { Screen, StatusBar, VirtualList, Scrollbar } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Queue actions" bg="theme.panel" deps={16} />
  <VirtualList x={12} y={36} w={284} h={196} count={5} text="Action " focus={true} />
  <Scrollbar x={306} y={36} w={4} h={196} count={5} fg="theme.accent" />
</Screen>
  );
}
