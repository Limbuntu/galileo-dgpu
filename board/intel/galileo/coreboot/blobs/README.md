# Blobs required to bring Quark X1000 out of reset

These are the two binaries coreboot's Quark port cannot boot without. They are
vendored here so the ROM build is offline and reproducible.

| File | Size | SHA-256 | License |
|---|---|---|---|
| `FSP_M.fd` | 192512 | `d25644adf8e02c149dde5e6b9ee467a63546d7395216d77293064da0cafb2350` | BSD (see below) |
| `FSP_S.fd` | 4096 | `ef7e21d09c7604969cbf0a7b946362afbb1f34f89f367b6d87078873ac9fc881` | BSD (see below) |
| `rmu.bin` | 8192 | `3bfdbdb47f38ebb426906c1e9e1bb8e7e42f25755a5b0e672e69a4649563f451` | Intel proprietary, binary redistribution allowed (see `RMU-LICENSE.txt`) |

Both FSP files carry a valid FSP 2.0 info header (`FSPH`, ImageId `QUK-FSP0`)
at offset `0x94`.

## FSP-M / FSP-S

Taken from the coreboot blobs repository, commit
`a63bd3805732f7077286ceddb0e1d164dc7d768e` (the revision that still carried
Quark blobs; they were removed after coreboot dropped the platform):

```
soc/intel/quark/Fsp2_0Pei/RELEASE/FSP_M.fd
soc/intel/quark/Fsp2_0Pei/RELEASE/FSP_S.fd
```

That repository's own README states these binaries were compiled from the
QuarkFsp sources, and that "the sources, as well as the edk2 sources, are
BSD-licensed, redistribution is not an issue."

To rebuild them from source instead:

```
git clone https://github.com/tianocore/edk2
git -C edk2 checkout a5cd3bb037cf87ecda0a5c8cd8a3eda722591b70
git clone https://review.gerrithub.io/LeeLeahy/quarkfsp QuarkFspPkg
patch -p1 -d QuarkFspPkg -i QuarkFsp.patch   # from the blobs repo
. edk2/edksetup.sh
QuarkFspPkg/BuildFsp2_0.sh -r32
QuarkFspPkg/BuildFsp2_0Pei.sh -r32
```

## rmu.bin

The Quark's Remote Management Unit firmware. Taken from the Intel Quark BSP
`Quark_EDKII_v1.0.1` at
`QuarkSocPkg/QuarkNorthCluster/Binary/QuarkMicrocode/RMU.bin` (8 KiB; the
`Quark2Microcode` variant is 16 KiB and is not used here). Intel's license
allows redistribution of the unmodified binary; the full text is in
`RMU-LICENSE.txt`.

`rmu.bin` is placed at `0xfff00000` (strap-determined address) by
`CONFIG_RMU_LOC`, which is why the image must never be written over the whole
SPI chip — see `../README.md`.
