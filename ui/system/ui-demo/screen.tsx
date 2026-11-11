import { Screen, StatusBar, Text, Dialog, Toast, CanvasSurface, PhotoSurface } from "@linnet/ui";
export default function UIDemo() {
  return (
    <Screen>
      <StatusBar w={220} h={20} text="UI Demo" bg="theme.panel" deps={16} />
      <Text x={8} y={26} w={204} h={20}>Edit TSX, build, deploy.</Text>
      <CanvasSurface x={8} y={52} w={96} h={44} fg="theme.accent" bg="theme.panel" deps={4} canvas="linnet.system.ui-demo.waveform" />
      <PhotoSurface x={112} y={52} w={100} h={44} resource="artwork" />
      <Dialog x={8} y={104} w={204} h={32} text="Compiled on the host" bg="theme.panel" flags={4} />
      <Toast x={8} y={144} w={204} h={24} text="No runtime parser" bg="theme.accent" fg="theme.background" />
    </Screen>
  );
}
