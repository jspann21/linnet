import { Screen, CanvasSurface } from "@linnet/ui";
export default function Scene() {
  return (<Screen bg="#07162b">
    <CanvasSurface x={0} y={0} w={320} h={240} canvas="linnet.system.game-play.draw" bg="#080429" deps={4} />
  </Screen>);
}
