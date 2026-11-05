# Surfacing the clips in Home Assistant (HAOS in a VM)

The receiver writes clips to a folder on the host; HA's Media browser serves
whatever is under `/media`. When HA runs as **HAOS in a VM** (so its `/media`
lives inside the VM), the cleanest route is:

## Option A — receiver on a host, mounted into HA via Network storage (recommended)

1. Run `clip_receiver.py` (or the container below) on an always-on host, writing
   to e.g. `/srv/cg721`.

   ```sh
   docker run -d --name cg721-receiver --restart unless-stopped \
     -p 8099:8099 -e CLIP_DIR=/data -e CLIP_KEEP=300 \
     -v /srv/cg721:/data \
     -v /path/to/clip_receiver.py:/app/clip_receiver.py:ro \
     python:3.12-alpine python /app/clip_receiver.py
   ```

2. Export the folder over NFS (Ubuntu host example):

   ```sh
   sudo apt install -y nfs-kernel-server
   echo "/srv/cg721 192.168.1.0/24(rw,sync,no_subtree_check,insecure,all_squash,anonuid=1000,anongid=1000)" \
     | sudo tee -a /etc/exports
   sudo exportfs -ra && sudo systemctl enable --now nfs-server
   # if ufw is active:
   sudo ufw allow from 192.168.1.0/24 to any port 2049 proto tcp
   sudo ufw allow from 192.168.1.0/24 to any port 111
   ```

3. In HA: **Settings → System → Storage → Add network storage**
   - Name `cg721` · Usage **Media** · Type **NFS**
   - Server `<host-ip>` · Remote path `/srv/cg721` · NFS v4

Clips then appear under **Media → cg721**, browsable and playable. Point the
camera's `app.sh` `RECV=` at the host running the receiver.

## Option B — receiver as a HAOS add-on (writes /media directly)

Copy `host/addon/cg721_receiver/` into the HAOS `/addons` share, then
Settings → Add-ons → Store → ⋮ → Check for updates → install "CG721 Clip
Receiver". It maps `media:rw` and uses host networking, so it listens on the HA
VM's own IP:8099 and writes straight to `/media/cg721`. Point `app.sh`
`RECV=` at the HA VM.

## Note on playback

The stock recorder encodes **H.265/HEVC**. Safari plays it inline; Chrome and
Firefox often cannot and show a black player (the file still lists/downloads).
To play in any browser, transcode to H.264 on the receiver (keep the original
alongside) — not included here.
