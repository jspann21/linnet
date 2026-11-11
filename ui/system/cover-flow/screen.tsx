import { Screen, CanvasSurface, Text } from "@linnet/ui";
export default function Scene() {
  return (<Screen bg="#000000">
    <CanvasSurface x={0} y={0} w={220} h={140} canvas="linnet.system.cover-flow.draw" bg="#000000" deps={4} />
    <Text x={4} y={143} w={212} h={16} text="Cover Flow album" fg="#ffffff" bg="#000000" />
    <Text x={4} y={160} w={212} h={14} text="Cover Flow artist" fg="#94a3b8" bg="#000000" />
  </Screen>);
}
