#!/bin/bash
set -e

sudo make -j24
sudo make modules_install
sudo make install

# Driver NVIDIA via dkms: a release string do kernel (uname -r) nao muda
# entre builds deste mesmo branch, entao o dkms acha que o modulo ja esta
# "instalado" pra essa release mesmo depois de um rebuild - o .ko fica com
# vermagic/simbolos do binario ANTERIOR, incompativel com o novo, e o
# grafico cai pra llvmpipe (renderizacao por software) ate isso ser
# refeito manualmente. Descobre a versao do nvidia instalada via dkms em
# vez de fixar, e usa a kernelrelease exata que acabou de ser buildada
# (nao uname -r, que so muda depois do reboot).
#
# Le include/config/kernel.release (gravado pelo build) e NAO recalcula com
# `make kernelrelease`: o sufixo "+" vem de scripts/setlocalversion e depende
# da arvore estar suja NO MOMENTO em que e avaliado. Recalcular aqui deu
# "7.0.0-rc1+" para um kernel que foi buildado e instalado como "7.0.0-rc1",
# e o dkms construiu o nvidia para a release errada.
KVER=$(cat include/config/kernel.release)
NVIDIA_VER=$(dkms status 2>/dev/null | grep -oP 'nvidia/\K[0-9.]+' | head -1)

if [ -z "$NVIDIA_VER" ]; then
	echo "nenhum modulo nvidia registrado no dkms - pulando driver de GPU" >&2
else
	echo "Reconstruindo nvidia/${NVIDIA_VER} para ${KVER}..."
	sudo dkms remove "nvidia/${NVIDIA_VER}" -k "${KVER}" 2>/dev/null || true
	sudo dkms install "nvidia/${NVIDIA_VER}" -k "${KVER}" --force
fi

echo "Kernel ${KVER} instalado, driver de GPU reconstruido."
echo "Reinicie para usar o novo kernel."
