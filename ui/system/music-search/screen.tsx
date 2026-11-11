import { Screen, StatusBar, Text, VirtualList, Scrollbar } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Search titles" bg="theme.panel" deps={16} />
  <Text x={4} y={22} w={212} h={18} text="Search prefix" />
  <VirtualList x={4} y={42} w={202} h={112} count={41} text="Search row" focus={true} />
  <Scrollbar x={210} y={42} w={6} h={112} count={41} fg="theme.accent" />
  <Text x={4} y={156} w={212} h={18} text="Search controls" />
</Screen>
  );
}
