#!/bin/bash
set -e

MACHINE_ID="7c6b088b521f449bbab32c3b40d117eb"
ESP="/efi/${MACHINE_ID}"

echo "Compilando kernel..."
make -j$(nproc)

VERSION=$(make -s kernelrelease)
ENTRY_DIR="${ESP}/${VERSION}"
CONF="/efi/loader/entries/${MACHINE_ID}-${VERSION}.conf"
echo "Versao: ${VERSION}"

echo "Instalando modulos..."
sudo make modules_install

echo "Instalando kernel em ${ENTRY_DIR}..."
sudo mkdir -p "$ENTRY_DIR"
sudo cp arch/x86/boot/bzImage "${ENTRY_DIR}/linux"
sudo dracut --force "${ENTRY_DIR}/initrd" "$VERSION"

sudo tee "$CONF" > /dev/null <<EOF
title   Linux ${VERSION}
version ${VERSION}
machine-id ${MACHINE_ID}
linux   /${MACHINE_ID}/${VERSION}/linux
initrd  /${MACHINE_ID}/${VERSION}/initrd
options nvme_load=YES nowatchdog rw nosmt=force root=UUID=94a96fe0-4ff2-4d7a-8a06-de97b249506a systemd.machine_id=${MACHINE_ID}
EOF

echo "Kernel ${VERSION} instalado."
echo "Entry: ${CONF}"
