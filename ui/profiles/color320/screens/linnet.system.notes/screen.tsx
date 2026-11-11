import { Screen, StatusBar, VirtualList, Scrollbar } from "@linnet/ui";
export default function Notes() { return (
<Screen>
  <StatusBar w={320} h={28} text="Notes" bg="theme.panel" deps={16} />
  <VirtualList x={12} y={36} w={284} h={196} count={0} text="Notes documents" focus={true} />
  <Scrollbar x={306} y={36} w={4} h={196} count={0} fg="theme.accent" />
</Screen>
); }
