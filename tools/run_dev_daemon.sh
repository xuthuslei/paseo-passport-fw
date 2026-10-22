#!/bin/bash
# 在独立 mount namespace 中运行联调 daemon：
# 把带 pr-worker.178960.xyz -> 127.0.0.1 的 hosts 文件 bind 覆盖 /etc/hosts，
# 使 daemon 的 relay 连接落到本地 CONNECT 隧道（connect_tunnel.py, :8443）。
# 用法：setsid nohup ~/workspace/paseo-passport-fw/tools/run_dev_daemon.sh > ~/workspace/paseo-passport-fw/tools/daemon-run.log 2>&1 &
exec unshare -m bash -c '
  mount --bind /tmp/hosts.new /etc/hosts || exit 1
  getent hosts pr-worker.178960.xyz
  exec /home/hatch/workspace/paseo-dev-daemon/node_modules/.bin/paseo daemon run --home /home/hatch/workspace/paseo-dev-daemon/home
'
