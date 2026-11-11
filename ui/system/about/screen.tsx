import { Screen, StatusBar, CanvasSurface, Text, ProgressBar, VirtualList, Scrollbar } from "@linnet/ui";
export default function Scene() {
  return (
    <Screen>
      <StatusBar w={220} h={20} text="About Linnet" bg="theme.panel" deps={16} />
      <CanvasSurface x={8} y={25} w={34} h={52} canvas="linnet.system.about.emblem" fg="theme.foreground" bg="theme.background" deps={4} />
      <Text x={50} y={34} w={160} h={20} text="LINNET OS" fg="theme.accent" />
      <Text x={50} y={57} w={160} h={16} text="Device details" fg="theme.muted" />
      <ProgressBar x={8} y={82} w={202} h={6} text="About storage" count={100} index={0} fg="theme.accent" bg="theme.panel" />
      <VirtualList x={4} y={94} w={202} h={62} count={3} text="About detail" focus={true} />
      <Scrollbar x={210} y={94} w={6} h={62} count={3} text="About detail" fg="theme.accent" />
      <Text x={4} y={159} w={212} h={16} text="Scroll  Menu: Back" fg="theme.muted" />
    </Screen>
  );
}
