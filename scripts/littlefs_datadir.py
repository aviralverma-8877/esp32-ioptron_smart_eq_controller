# pre: extra_script for [env:esp32dev-littlefs] only. PlatformIO's "data"
# folder for `buildfs`/`uploadfs` (PROJECT_DATA_DIR) is a single project-wide
# setting, not per-environment - override it here so this one env packs
# tools/littlefs/data/ instead, without touching the project-wide default
# (which the plain "esp32dev" / SD-card env never uses anyway).
Import("env")
import os

env.Replace(PROJECT_DATA_DIR=os.path.join(
    env.subst("$PROJECT_DIR"), "tools", "littlefs", "data"))
