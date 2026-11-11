import { Screen, StatusBar, Text, VirtualList, Scrollbar } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Search titles" bg="theme.panel" deps={16} />
  <Text x={12} y={36} w={296} h={24} text="Search prefix" />
  <VirtualList x={12} y={68} w={284} h={140} count={41} text="Search row" focus={true} />
  <Scrollbar x={306} y={68} w={4} h={140} count={41} fg="theme.accent" />
  <Text x={12} y={216} w={296} h={20} text="Search controls" />
</Screen>
  );
}
