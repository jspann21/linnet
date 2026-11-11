import { Screen, StatusBar, Menu, Scrollbar, Text } from "@linnet/ui";
export default function Scene() {
  return (<Screen>
    <StatusBar w={220} h={20} text="Games" bg="theme.panel" />
    <Menu x={8} y={27} w={204} h={105} focus={true} text="Game choices"
      items={['Comet Run', 'Pulse Path', 'Solitaire', 'Sudoku']} targets={['linnet.system.game-play', 'linnet.system.game-play', 'linnet.system.game-play', 'linnet.system.game-play']} />
    <Scrollbar x={210} y={27} w={6} h={105} fg="theme.accent" />
    <Text x={8} y={132} w={204} h={22} text="Game description" fg="theme.muted" deps={2} />
    <Text x={8} y={158} w={204} h={14} text="Game music" fg="theme.muted" deps={16} />
  </Screen>);
}
