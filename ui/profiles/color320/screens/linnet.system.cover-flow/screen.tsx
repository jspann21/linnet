import { Screen, CanvasSurface, Text } from "@linnet/ui";
export default function Scene() {
  return (<Screen bg="#000000">
    <CanvasSurface x={0} y={0} w={320} h={190} canvas="linnet.system.cover-flow.draw" bg="#000000" deps={4} />
    <Text x={5} y={195} w={308} h={21} text="Cover Flow album" fg="#ffffff" bg="#000000" />
    <Text x={5} y={218} w={308} h={19} text="Cover Flow artist" fg="#94a3b8" bg="#000000" />
  </Screen>);
}
