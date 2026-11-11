import { Screen, StatusBar, Text, Menu } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={320} h={28} text="Firmware Info" bg="theme.panel" deps={16} />
  <Text x={5} y={32} w={308} h={27} text="Update build" />
  <Text x={5} y={61} w={308} h={27} text="Update saved" />
  <Text x={5} y={90} w={308} h={27} text="Update candidate" />
  <Text x={5} y={118} w={308} h={27} text="Update health" />
  <Text x={5} y={147} w={308} h={27} text="Update history" fg="theme.muted" />
  <Menu x={5} y={182} w={308} h={51} focus={true}
    items={['USB Disk Mode']}
    targets={['@sync']} />
</Screen>
  );
}
