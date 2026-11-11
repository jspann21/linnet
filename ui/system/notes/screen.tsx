import { Screen, StatusBar, VirtualList, Scrollbar } from "@linnet/ui";
export default function Notes() { return (
<Screen>
  <StatusBar w={220} h={20} text="Notes" bg="theme.panel" deps={16} />
  <VirtualList x={4} y={24} w={202} h={148} count={0} text="Notes documents" focus={true} />
  <Scrollbar x={210} y={24} w={6} h={148} count={0} fg="theme.accent" />
</Screen>
); }
