#!bin/bash

if [ $1 -eq 0 ];
then
  echo "Disable DDIO"
  for PCI_ADDR in `lspci| grep PCI| grep bridge| cut -d' ' -f1`
  do
      setpci -s $PCI_ADDR 180.B=51
  done
fi

if [ $1 -eq 1 ];
then
    echo "Enable DDIO"
    for PCI_ADDR in `lspci| grep PCI| grep bridge| cut -d' ' -f1`
    do
        setpci -s $PCI_ADDR 180.B=d1
    done
fi

