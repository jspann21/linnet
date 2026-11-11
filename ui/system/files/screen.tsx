import { Screen, StatusBar, VirtualList, Scrollbar, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Files" bg="theme.panel" deps={16} />
  <VirtualList x={4} y={24} w={202} h={132} count={1} text="Loading..." focus={true} />
  <Scrollbar x={210} y={24} w={6} h={132} count={1} fg="theme.accent" />
  <Text x={4} y={160} w={212} h={16} text="Select: Open  Menu: Back" fg="theme.muted" />
</Screen>
  );
}
