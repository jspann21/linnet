import { Screen, StatusBar, VirtualList, Scrollbar, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Sound" bg="theme.panel" deps={16} />
  <VirtualList x={4} y={24} w={202} h={124} count={6} text="Sound " focus={true} />
  <Scrollbar x={210} y={24} w={6} h={124} count={6} fg="theme.accent" />
  <Text x={4} y={153} w={212} h={20} text="Select: Open / Change" fg="theme.muted" />
</Screen>
  );
}
