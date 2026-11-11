import { Screen, VideoSurface, StatusBar, ProgressBar, Text } from "@linnet/ui";
export default function Scene() { return (<Screen><VideoSurface x={0} y={0} w={220} h={176} text="" /><StatusBar w={220} h={20} text="Video" bg="theme.panel" /><ProgressBar x={8} y={152} w={204} h={4} count={1000} /><Text x={8} y={160} w={204} h={16} text="MENU Exit  PLAY Pause  < > Seek" /></Screen>); }
