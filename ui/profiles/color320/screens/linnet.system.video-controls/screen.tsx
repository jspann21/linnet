import { Screen, VideoSurface, StatusBar, ProgressBar, Text } from "@linnet/ui";
export default function Scene() { return (<Screen><VideoSurface x={0} y={0} w={320} h={240} text="" /><StatusBar w={320} h={24} text="Video" bg="theme.panel" /><ProgressBar x={8} y={208} w={304} h={4} count={1000} /><Text x={8} y={216} w={304} h={19} text="MENU Exit  PLAY Pause  < > Seek" /></Screen>); }
