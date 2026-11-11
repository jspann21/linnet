import { Screen, StatusBar, Menu, Scrollbar } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Settings" bg="theme.panel" deps={16} />
  <Menu x={4} y={24} w={202} h={148} focus={true}
    items={['Display','Themes','Date & Time','Battery','Power','Sleep Timer','USB Disk Mode','Firmware Info','About Linnet','Back to Home','Shuffle: Off','Repeat: Off','Sound','Video Settings','Edit Menus']}
    targets={['linnet.system.display','linnet.system.themes','linnet.system.clock','linnet.system.battery','linnet.system.shutdown','linnet.system.sleep-timer','@sync','linnet.system.update','linnet.system.about','linnet.system.home','@shuffle','@repeat','linnet.system.music-sound','linnet.system.video-settings', '@menus']} />
  <Scrollbar x={210} y={24} w={6} h={148} count={12} fg="theme.accent" />
</Screen>
  );
}
