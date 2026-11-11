import { Screen, StatusBar, VirtualList, Text } from "@linnet/ui";
export default function Scene() { return (<Screen><StatusBar w={320} h={28} text="Play video on" bg="theme.panel" /><VirtualList x={16} y={58} w={288} h={92} count={2} text="Video output" focus={true} /><Text x={16} y={178} w={288} h={28} text="Select an output" fg="theme.muted" /></Screen>); }
