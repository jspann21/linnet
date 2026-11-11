import { Screen, StatusBar, VirtualList, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Power" bg="theme.panel" deps={16} />
  <VirtualList x={4} y={24} w={212} h={72} count={3} text="Automatic shutdown" items={['Headphones out: Off','Headphones in: Off','Battery Capacity']} focus={true} />
  <Text x={4} y={102} w={212} h={20} text="Auto off includes playback" fg="theme.muted" />
  <Text x={4} y={128} w={212} h={20} text="SELECT/RIGHT Next" fg="theme.muted" />
  <Text x={4} y={152} w={212} h={20} text="LEFT Previous" fg="theme.muted" />
</Screen>
  );
}
