import { Screen, StatusBar, VirtualList, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Battery" bg="theme.panel" deps={16} />
  <VirtualList x={5} y={32} w={308} h={43} count={1} text="Battery capacity" items={['Capacity: 1200 mAh']} focus={true} />
  <Text x={5} y={92} w={308} h={27} text="Installed battery rating" fg="theme.muted" />
  <Text x={5} y={147} w={308} h={27} text="SELECT/RIGHT +50 mAh" fg="theme.muted" />
  <Text x={5} y={180} w={308} h={27} text="LEFT -50 mAh" fg="theme.muted" />
</Screen>
  );
}
