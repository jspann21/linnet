import { Screen, StatusBar, VirtualList, Text } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Battery" bg="theme.panel" deps={16} />
  <VirtualList x={4} y={24} w={212} h={32} count={1} text="Battery capacity" items={['Capacity: 1200 mAh']} focus={true} />
  <Text x={4} y={68} w={212} h={20} text="Installed battery rating" fg="theme.muted" />
  <Text x={4} y={108} w={212} h={20} text="SELECT/RIGHT +50 mAh" fg="theme.muted" />
  <Text x={4} y={132} w={212} h={20} text="LEFT -50 mAh" fg="theme.muted" />
</Screen>
  );
}
