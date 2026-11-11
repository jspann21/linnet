import { Screen, StatusBar, VirtualList, Scrollbar, Text } from "@linnet/ui";
export default function Scene() { return (
<Screen>
  <StatusBar w={320} h={28} text="Clock edit title" deps={16} />
  <VirtualList x={5} y={32} w={293} h={147} count={6} text="Clock fields" focus={true}
    items={['Year','Month','Day','Hour','Minute','Save date and time']} />
  <Scrollbar x={305} y={32} w={8} h={147} count={6} fg="theme.accent" />
  <Text x={5} y={190} w={308} h={43} text="Clock edit hint" fg="theme.muted" />
</Screen>); }
