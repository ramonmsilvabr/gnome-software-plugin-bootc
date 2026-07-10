# gnome-software-plugin-bootc

A native GNOME Software plugin for managing system updates on systems deployed with `bootc`.

## How it works

Because GNOME Software runs in the user session and `bootc` requires root privileges to operate, this plugin invokes a small privileged helper script (`gs-bootc-helper`) via `pkexec` to bridge the update progress pipeline. 

A PolicyKit rule is provided to allow background passwordless update checks (`bootc upgrade --check`) and status queries, while keeping the actual installation protected by standard authentication.

## Building

You can build the plugin using Meson:

```sh
meson setup builddir
meson compile -C builddir
```

## Installation

### 1. The Plugin
Install `libgs_plugin_bootc.so` into the GNOME Software plugins directory (typically `/usr/lib64/gnome-software/plugins-23/`).

### 2. Privileged Helper and Polkit Rules
Copy the helper script and PolicyKit configurations to their respective system paths:

```sh
# Helper script
cp sys-utils/gs-bootc-helper /usr/libexec/gs-bootc-helper
chmod 755 /usr/libexec/gs-bootc-helper
chown root:root /usr/libexec/gs-bootc-helper

# Polkit rules (allows background passwordless checks)
cp sys-utils/99-bootc-check.rules /usr/share/polkit-1/rules.d/
chmod 644 /usr/share/polkit-1/rules.d/99-bootc-check.rules
chown root:root /usr/share/polkit-1/rules.d/99-bootc-check.rules

# Polkit policy (custom password dialog prompt)
cp sys-utils/org.containers.bootc.policy /usr/share/polkit-1/actions/
chmod 644 /usr/share/polkit-1/actions/org.containers.bootc.policy
chown root:root /usr/share/polkit-1/actions/org.containers.bootc.policy
```

## ComposeFS Limitation

On systems booted with ComposeFS (as opposed to the traditional `ostree-container` backend), `bootc` does not currently support writing progress data to `--progress-fd`. 

To handle this, the plugin automatically detects the active storage mode from `bootc status`. On ComposeFS systems, it falls back to reporting `GS_APP_PROGRESS_UNKNOWN` during updates. This causes GNOME Software to display a pulsing activity indicator rather than a frozen progress bar, switching to 100% when the update finishes staging.

## License

This project is licensed under the GPL-2.0 License.

