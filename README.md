# Thingy91 Example App

This application is based on the Nordic Asset Tracker app which can be found in
NCS under `applications/asset_tracker_v2`. This specific app was forked from
[the out-of-tree version of this application](https://github.com/NordicSemiconductor/asset-tracker-cloud-firmware-aws).

## Setup

Follow the [Getting Started with the Thingy91](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/device_guides/working_with_nrf/nrf91/thingy91_gsg.html)
instructions for updating the modem firmware, activating the SIM card, and
registering the device with nRF Cloud.

Install Zephyr's dependencies [here](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).

Create a workspace folder:

```bash
mkdir ~/thingy91-workspace
cd thingy91-workspace
```

Create and activate a virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
```

Install west, Zephyr's meta-tool:

```bash
pip install west
```

Use west to initialize the workspace with our asset-tracker repo as the
manifest repo and then update the modules from the manifest
`memfault-asset-tracker/west.yml`:

```bash
cd ~/thingy91-workspace
west init -m git@github.com:memfault/memfault-asset-tracker
west update
```

Install Python requirements for both Zephyr and NCS:

```bash
pip install -r zephyr/scripts/requirements.txt -r nrf/scripts/requirements-build.txt
```

## Build & Flash

Build the app with your project key and the Memfault overlay:

```bash
cd ~/thingy91-workspace
west build -b thingy91_nrf9160_ns -p always memfault-asset-tracker -- \
-DCONFIG_MEMFAULT_NCS_PROJECT_KEY=\"${MEMFAULT_PROJECT_KEY}\" \
-DOVERLAY_CONFIG=overlay-memfault.conf
```

Flash:

```bash
west flash --erase
```

## Deploying OTAs

This app also has an example of how to auto-deploy releases with Memfault.
See [`ota-deploy.yaml`](.github/workflows/ota-deploy.yaml).
