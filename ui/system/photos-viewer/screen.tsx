import { Screen, StatusBar, PhotoSurface } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Photo viewer" bg="theme.panel" deps={16} />
  <PhotoSurface x={0} y={20} w={220} h={156} text="" />
</Screen>
  );
}
