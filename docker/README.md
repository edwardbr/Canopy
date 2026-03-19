# Docker Development Image

[`create_strix_halo_toolbox.sh`](/var/home/edward/projects/Canopy2/docker/create_strix_halo_toolbox.sh) builds the local image from [`Dockerfile.strix_halo`](/var/home/edward/projects/Canopy2/docker/Dockerfile.strix_halo) and creates the Toolbox container.

Run:

```bash
./docker/create_strix_halo_toolbox.sh
```

Defaults:

```bash
IMAGE_NAME=localhost/canopy-strix-halo:rocm7-nightlies
TOOLBOX_NAME=canopy-strix-halo
```

The script passes:

```bash
--device /dev/dri
--device /dev/kfd
--group-add video
--group-add render
--group-add sudo
--security-opt seccomp=unconfined
```

For Fedora Silverblue Toolbox usage, Toolbox should enter the container as your host user, for example `edward`.
