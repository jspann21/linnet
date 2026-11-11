import { Screen, CanvasSurface } from "@linnet/ui";
export default function Scene() {
  return (<Screen bg="#07162b">
    <CanvasSurface x={0} y={0} w={220} h={176} canvas="linnet.system.game-play.draw" bg="#07162b" deps={4} />
  </Screen>);
}
