import { Screen, StatusBar, Scrollbar, Menu } from "@linnet/ui";
export default function Scene() { return (
<Screen>
  <StatusBar w={220} h={20} text="Extras" bg="theme.panel" deps={16} />
  <Menu x={4} y={24} w={202} h={148} items={['Games','Notes','Date & Time','Sleep Timer','Videos','Files','UI Demo']} targets={['linnet.system.games','linnet.system.notes','linnet.system.clock','linnet.system.sleep-timer','linnet.system.videos','linnet.system.files','linnet.system.ui-demo']} focus={true} />
  <Scrollbar x={210} y={24} w={6} h={148} fg="theme.accent" />
</Screen>
); }
