# Openbox — NetLinux build

A fork of [danakj/openbox](https://github.com/danakj/openbox) packaged for the
NetLinux package repository, carrying a **snap layouts** patch on top of
upstream.

**This package:**
<https://packages.netlinux.co.uk/debian/pool/main/o/openbox/> — built for both
`stable` (Debian 12) and `resolute` (Ubuntu 26.04) from the `netlinux` branch.

**All NetLinux packages:** <https://packages.netlinux.co.uk/> — browse the full
package set, and the apt source and GPG key needed to install any of them.

## Install

Set up the apt source and GPG key as described on
[packages.netlinux.co.uk](https://packages.netlinux.co.uk/), then:

```sh
sudo apt install openbox
```

Note that the distros ship openbox `3.6.1`, which apt sorts above this build's
`3.6-<build>netlinux1`, so a plain install prefers the distro package. Pin it to
get this one:

```sh
sudo tee /etc/apt/preferences.d/netlinux-openbox >/dev/null <<'EOF'
Package: openbox
Pin: release o=NetLinux
Pin-Priority: 1001
EOF
sudo apt update && sudo apt install openbox
```

## What's added: snap layouts

Hovering a window's maximize button pops up a grid of layout templates, in the
manner of Windows 11. Each template is a miniature of the monitor split into
zones; the zone under the pointer highlights, and clicking it moves and resizes
the window to fill that part of the monitor's usable area.

Six layouts ship by default — halves, thirds, two-thirds + third, half + two
stacked, quadrants, and top/bottom. Zones are measured against the monitor the
window is on, minus any panels or docks reserving space there.

Configure it with a `<snapLayouts>` block in `~/.config/openbox/rc.xml`, then
run `openbox --reconfigure`:

```xml
<snapLayouts>
  <enabled>yes</enabled>
  <delay>400</delay>
  <layout>
    <zone x="0" y="0" width="50" height="100"/>
    <zone x="50" y="0" width="50" height="100"/>
  </layout>
</snapLayouts>
```

Zone positions and sizes are percentages of the monitor's usable area. Listing
any `<layout>` replaces the built-in set entirely. The implementation lives in
[`openbox/snap.c`](openbox/snap.c).

## Branches

| Branch | Purpose |
| --- | --- |
| `netlinux` | The packaged branch. Upstream plus the snap layouts patch and the NetLinux CI workflows; every push builds and publishes both suites. |
| `upstream-snap-layouts` | The same patch rebased onto current upstream `master`, without the CI commits — the form intended for submission upstream. |

## Building from source

```sh
./bootstrap
./configure --prefix=/usr --sysconfdir=/etc
make
```

## Testing without installing

`openbox` takes `--config-file FILE` to run against a config other than
`~/.config/openbox/rc.xml` or the system default, which is useful for trying
out `rc.xml` changes (like editing `<snapLayouts>`) without touching either.
Run it inside an `Xnest`/`Xephyr` nested display so it doesn't disturb your
real session:

```sh
Xnest :3 -geometry 1280x800 &
DISPLAY=:3 ./openbox/openbox --config-file data/rc.xml &
DISPLAY=:3 xterm &   # or any client, to have a window to test against
```

`--config-file` only takes effect at startup, not on `--reconfigure`. After
editing the XML again, reload it with:

```sh
DISPLAY=:3 ./openbox/openbox --reconfigure
```

which re-reads from the same path the instance was started with.

Upstream's original README, covering the project itself, is in
[`README`](README); see also [`README.GIT`](README.GIT) and
[`README.NLS`](README.NLS).
