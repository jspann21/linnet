import { Screen, StatusBar, Scrollbar, Menu } from "@linnet/ui";
export default function Scene() {
  return (
    <Screen>
      <StatusBar w={320} h={28} text="Extras" bg="theme.panel" deps={16} />
      <Menu x={12} y={36} w={284} h={196} focus={true}
        items={['Games','Notes','Date & Time','Sleep Timer','Videos','Files','UI Demo']}
        targets={['linnet.system.games','linnet.system.notes','linnet.system.clock','linnet.system.sleep-timer','linnet.system.videos','linnet.system.files','linnet.system.ui-demo']} />
      <Scrollbar x={306} y={36} w={4} h={196} fg="theme.accent" />
    </Screen>
  );
}
