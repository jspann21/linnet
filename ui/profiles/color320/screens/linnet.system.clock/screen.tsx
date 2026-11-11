import { Screen, StatusBar, VirtualList, Scrollbar, Text } from "@linnet/ui";
export default function Scene() { return (
<Screen>
  <StatusBar w={320} h={28} text="Date & Time" bg="theme.panel" deps={16} />
  <Text x={11} y={40} w={296} h={46} text="System clock" />
  <VirtualList x={5} y={106} w={293} h={65} count={2} text="Clock settings" focus={true}
    items={['Set date and time','Time format: 24-hour']} />
  <Text x={11} y={190} w={296} h={43} text="Clock source" fg="theme.muted" />
  <Scrollbar x={305} y={106} w={8} h={65} fg="theme.accent" />
</Screen>
); }
