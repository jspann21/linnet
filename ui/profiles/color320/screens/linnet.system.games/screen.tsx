import { Screen, StatusBar, Menu, Scrollbar, Text } from "@linnet/ui";
export default function Scene() {
  return (<Screen>
    <StatusBar w={320} h={28} text="Games" bg="theme.panel" deps={16} />
    <Menu x={12} y={36} w={284} h={158} focus={true} text="Game choices"
      items={['Comet Run', 'Pulse Path', 'Solitaire', 'Sudoku']} targets={['linnet.system.game-play', 'linnet.system.game-play', 'linnet.system.game-play', 'linnet.system.game-play']} />
    <Scrollbar x={306} y={36} w={4} h={158} fg="theme.accent" />
    <Text x={12} y={198} w={296} h={18} text="Game music" fg="theme.muted" deps={16} />
    <Text x={12} y={218} w={296} h={14} text="Game description" fg="theme.foreground" deps={2} />
  </Screen>);
}
