# my_ice_rust

Rust binary for running the `my_ice` VFIO userspace driver path.

## Current implementation

`my_ice_rust` is a Rust entrypoint that links to the existing C driver core (`main.c`) as a native static library and calls it via FFI. This gives you a Rust binary now, with the same driver behavior and CLI as `my_ice`.

## Build

```bash
bash /users/manvik12/my_ice/my_ice_rust/build.sh
```

## Run

```bash
/users/manvik12/my_ice/my_ice_rust/my_ice_rust <BDF> --tx-bench 10 <dst-mac> 1000
```

## Compare all three

Use:

```bash
sudo /users/manvik12/my_ice/dpdk_compare.sh --bdf <BDF> --dst-mac <dst-mac> --seconds 10 --payload-len 1000
```
