import { Screen, StatusBar, AlbumArt, Text, ProgressBar } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Player" bg="theme.panel" deps={16} />
  <AlbumArt x={8} y={28} w={64} h={64} resource="artwork" />
  <Text x={80} y={30} w={132} h={30} text="Sample track" />
  <Text x={80} y={65} w={132} h={28} text="Audio pending" fg="theme.muted" />
  <ProgressBar x={8} y={112} w={204} h={10} count={100} index={35} fg="theme.accent" bg="theme.panel" deps={4} />
  <Text x={30} y={136} w={182} h={22} text="Native playback shell" />
</Screen>
  );
}
