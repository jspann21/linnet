import { Screen, StatusBar, CanvasSurface, Text, ProgressBar, VirtualList, Scrollbar } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="About Linnet" bg="theme.panel" deps={16} />
  <CanvasSurface x={12} y={32} w={44} h={68} canvas="linnet.system.about.emblem" fg="theme.foreground" bg="theme.background" deps={4} />
  <Text x={66} y={45} w={242} h={24} text="LINNET OS" fg="theme.accent" />
  <Text x={66} y={74} w={242} h={20} text="Device details" fg="theme.muted" />
  <ProgressBar x={12} y={104} w={296} h={8} text="About storage" count={100} index={0} fg="theme.accent" bg="theme.panel" />
  <VirtualList x={4} y={120} w={302} h={98} count={3} text="About detail" focus={true} />
  <Scrollbar x={310} y={120} w={6} h={98} count={3} text="About detail" fg="theme.accent" />
  <Text x={4} y={222} w={312} h={16} text="Scroll  Menu: Back" fg="theme.muted" />
</Screen>
  );
}
