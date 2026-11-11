import { Screen, StatusBar, Text, Dialog, Toast, CanvasSurface, PhotoSurface } from "@linnet/ui";
export default function UIDemo() {
  return (
    <Screen>
      <StatusBar w={320} h={28} text="UI Demo" bg="theme.panel" deps={16} />
      <Text x={11} y={35} w={296} h={27}>Edit TSX, build, deploy.</Text>
      <CanvasSurface x={11} y={70} w={139} h={60} fg="theme.accent" bg="theme.panel" deps={4} canvas="linnet.system.ui-demo.waveform" />
      <PhotoSurface x={162} y={70} w={145} h={60} resource="artwork" />
      <Dialog x={11} y={141} w={296} h={43} text="Compiled on the host" bg="theme.panel" flags={4} />
      <Toast x={11} y={196} w={296} h={32} text="No runtime parser" bg="theme.accent" fg="theme.background" />
    </Screen>
  );
}
