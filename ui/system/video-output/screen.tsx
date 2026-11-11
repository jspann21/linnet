import { Screen, StatusBar, VirtualList, Text } from "@linnet/ui";
export default function Scene() { return (<Screen><StatusBar w={220} h={20} text="Play video on" bg="theme.panel" /><VirtualList x={8} y={38} w={204} h={76} count={2} text="Video output" focus={true} /><Text x={8} y={132} w={204} h={20} text="Select an output" fg="theme.muted" /></Screen>); }
