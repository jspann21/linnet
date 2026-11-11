import { Screen, StatusBar, Scrollbar, Menu } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar x={0} y={0} w={320} h={28} text="Linnet" bg="theme.panel" deps={16} />
  <Menu x={12} y={36} w={284} h={196} focus={true}
    items={['Music','Shuffle Songs','Cover Flow','Photos','Videos','Extras','Now Playing','Settings','UI Demo','Notes','Files']}
    targets={['linnet.system.music','@shuffle-all','linnet.system.cover-flow','linnet.system.photos','linnet.system.videos','linnet.system.extras','linnet.system.now-playing','linnet.system.settings','linnet.system.ui-demo','linnet.system.notes','linnet.system.files']} />
  <Scrollbar x={306} y={36} w={4} h={196} fg="theme.accent" />
</Screen>
  );
}
