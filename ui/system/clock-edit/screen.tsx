import { Screen, StatusBar, VirtualList, Scrollbar, Text } from "@linnet/ui";
export default function Scene() { return (
<Screen>
  <StatusBar w={220} h={20} text="Clock edit title" deps={16} />
  <VirtualList x={4} y={24} w={202} h={108} count={6} text="Clock fields" focus={true}
    items={['Year','Month','Day','Hour','Minute','Save date and time']} />
  <Scrollbar x={210} y={24} w={6} h={108} count={6} fg="theme.accent" />
  <Text x={4} y={140} w={212} h={32} text="Clock edit hint" fg="theme.muted" />
</Screen>); }
