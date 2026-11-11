import { Screen, StatusBar, Text, Menu } from "@linnet/ui";
export default function Scene() {
  return (
<Screen>
  <StatusBar w={220} h={20} text="Firmware Info" bg="theme.panel" deps={16} />
  <Text x={4} y={24} w={212} h={20} text="Update build" />
  <Text x={4} y={45} w={212} h={20} text="Update saved" />
  <Text x={4} y={66} w={212} h={20} text="Update candidate" />
  <Text x={4} y={87} w={212} h={20} text="Update health" />
  <Text x={4} y={108} w={212} h={20} text="Update history" fg="theme.muted" />
  <Menu x={4} y={134} w={212} h={38} focus={true}
    items={['USB Disk Mode']}
    targets={['@sync']} />
</Screen>
  );
}
