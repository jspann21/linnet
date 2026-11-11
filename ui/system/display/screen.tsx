import { Screen, StatusBar, VirtualList, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Display" bg="theme.panel" deps={16} />
  <VirtualList x={4} y={24} w={212} h={66} count={3} text="Display timeout" items={['Brightness: 100%','Backlight: 30 sec','Themes']} focus={true} />
  <Text x={4} y={108} w={212} h={20} text="Display hint" fg="theme.muted" />
  <Text x={4} y={132} w={212} h={20} text="Display status" fg="theme.muted" />
</Screen>
  );
}
