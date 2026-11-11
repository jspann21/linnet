import { Screen, StatusBar, Text, Menu } from "@linnet/ui";
export default function Starter() {
  return (
    <Screen>
      <StatusBar x={0} y={0} w={220} h={20} text="Starter" bg="theme.panel" deps={16} />
      <Text x={8} y={30} w={204} h={20} fg="theme.accent">Hello, Linnet!</Text>
      <Text x={8} y={56} w={204} h={20}>Edit apps/starter.</Text>
      <Menu x={4} y={100} w={212} h={44} focus={true}
        items={['Home']} targets={['linnet.system.home']} />
    </Screen>
  );
}
