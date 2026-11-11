import { Screen, StatusBar, Scrollbar, Menu } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar x={0} y={0} w={220} h={20} text="Linnet" bg="theme.panel" deps={16} />
  <Menu x={4} y={24} w={202} h={148} focus={true}
    items={['Music','Shuffle Songs','Cover Flow','Photos','Videos','Extras','Now Playing','Settings','UI Demo','Notes','Files']}
    targets={['linnet.system.music','@shuffle-all','linnet.system.cover-flow','linnet.system.photos','linnet.system.videos','linnet.system.extras','linnet.system.now-playing','linnet.system.settings','linnet.system.ui-demo','linnet.system.notes','linnet.system.files']} />
  <Scrollbar x={210} y={24} w={6} h={148} fg="theme.accent" />
</Screen>
  );
}
