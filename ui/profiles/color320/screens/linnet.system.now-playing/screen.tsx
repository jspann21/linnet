import { Screen, StatusBar, AlbumArt, Text, ProgressBar } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Now Playing" bg="theme.panel" deps={16} />
  <AlbumArt x={12} y={44} w={112} h={112} resource="artwork" />
  <Text x={138} y={44} w={170} h={48} text="Sample track" />
  <Text x={138} y={100} w={170} h={40} text="Audio pending" fg="theme.muted" />
  <Text x={12} y={166} w={296} h={20} text="Track album" fg="theme.muted" />
  <ProgressBar x={12} y={196} w={296} h={6} count={100} index={35} fg="theme.accent" bg="theme.panel" deps={4} />
  <Text x={12} y={212} w={296} h={20} text="Native playback shell" />
</Screen>
  );
}
