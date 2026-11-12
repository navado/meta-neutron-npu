# Neutron NPU (i.MX95): build NPU buffers from 2 MiB chunks of the Neutron CMA pool behind the SMMU instead of
# one physically contiguous block; fix the cache-sync race, buffer references, CACHE_SYNC range check and a
# double free. See README.md. Needs CONFIG_NEUTRON=y (the default in imx_v8_defconfig and Compulab's config).
#
# NEUTRON_NPU_PATCH picks the variant for your kernel revision:
#   ...-lf-6.18.20.patch  linux-imx lf-6.18.20 (meta-imx lf-6.18.20-2.0.0) and linux-compulab_v6.18.20
#   ...-lf-6.18.y.patch   linux-imx lf-6.18.y after the 2026-07/08 mailbox handshake rework
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../patches:"
NEUTRON_NPU_PATCH ?= "0001-staging-neutron-2MiB-CMA-chunks-lf-6.18.20.patch"
SRC_URI:append:mx95-nxp-bsp = " file://${NEUTRON_NPU_PATCH}"
