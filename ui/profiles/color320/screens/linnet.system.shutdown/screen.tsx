import { Screen, StatusBar, VirtualList, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Power" bg="theme.panel" deps={16} />
  <VirtualList x={5} y={32} w={308} h={84} count={3} text="Automatic shutdown" items={['Headphones out: Off','Headphones in: Off','Battery Capacity']} focus={true} />
  <Text x={5} y={125} w={308} h={27} text="Auto off includes playback" fg="theme.muted" />
  <Text x={5} y={174} w={308} h={27} text="SELECT/RIGHT Next" fg="theme.muted" />
  <Text x={5} y={207} w={308} h={27} text="LEFT Previous" fg="theme.muted" />
</Screen>
  );
}
