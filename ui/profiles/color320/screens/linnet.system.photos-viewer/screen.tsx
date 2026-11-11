import { Screen, StatusBar, PhotoSurface } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Photo viewer" bg="theme.panel" deps={16} />
  <PhotoSurface x={0} y={28} w={320} h={212} text="" />
</Screen>
  );
}
