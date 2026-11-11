import { Screen, StatusBar, VirtualList, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Display" bg="theme.panel" deps={16} />
  <VirtualList x={5} y={32} w={308} h={110} count={3} text="Display timeout" items={['Brightness: 100%','Backlight: 30 sec','Themes']} focus={true} />
  <Text x={5} y={147} w={308} h={27} text="Display hint" fg="theme.muted" />
  <Text x={5} y={180} w={308} h={27} text="Display status" fg="theme.muted" />
</Screen>
  );
}
