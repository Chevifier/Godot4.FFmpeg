import os
import pathlib
import shutil
import urllib.request
import tarfile
import SCons

FFMPEG_DOWNLOAD_WIN64 = "https://github.com/EIRTeam/FFmpeg-Builds/releases/download/autobuild-2023-07-24-08-52/ffmpeg-N-111611-g5b11ee9429-win64-lgpl-godot.tar.xz"
FFMPEG_DOWNLOAD_LINUX64 = "https://github.com/EIRTeam/FFmpeg-Builds/releases/download/autobuild-2023-07-24-08-52/ffmpeg-N-111611-g5b11ee9429-linux64-lgpl-godot.tar.xz"
ffmpeg_versions = {
    "avcodec": "60",
    "avfilter": "9",
    "avformat": "60",
    "avutil": "58",
    "swresample": "4",
    "swscale": "7",
}


def get_ffmpeg_install_targets(env, target_dir):
    return [os.path.join(target_dir, f"lib{k}.so.{v}") for k, v in ffmpeg_versions.items()]


def get_ffmpeg_install_sources(env, target_dir):
    return [os.path.join(target_dir, f"lib/lib{k}.so") for k in ffmpeg_versions]


def get_download_url(env):
    if env["platform"] == "linuxbsd" or env["platform"] == "linux":
        FFMPEG_DOWNLOAD_URL = FFMPEG_DOWNLOAD_LINUX64
    else:
        FFMPEG_DOWNLOAD_URL = FFMPEG_DOWNLOAD_WIN64
    return FFMPEG_DOWNLOAD_URL


def download_ffmpeg(target, source, env):
    dst = os.path.dirname(target[0])
    if os.path.exists(dst):
        shutil.rmtree(dst)

    FFMPEG_DOWNLOAD_URL = get_download_url(env)

    local_filename, headers = urllib.request.urlretrieve(FFMPEG_DOWNLOAD_URL)

    def rewrite_subfolder_paths(tf, common_path):
        l = len(common_path)
        for member in tf.getmembers():
            if member.path.startswith(common_path):
                member.path = member.path[l:]
                yield member

    with tarfile.open(local_filename, mode="r") as f:
        # Get the first folder
        common_path = os.path.commonpath(f.getnames()) + "/"
        f.extractall(dst, members=rewrite_subfolder_paths(f, common_path))
        os.remove(local_filename)


def _ffmpeg_emitter(target, source, env):
    target += get_ffmpeg_install_sources(env, os.path.dirname(target[0].get_path()))
    return target, source


def ffmpeg_download_builder(env, target, source):
    bkw = {
        "action": env.Run(download_ffmpeg, "Downloading FFMPEG library"),
        "target_factory": env.fs.Entry,
        "source_factory": env.fs.Entry,
        "emitter": _ffmpeg_emitter,
    }

    bld = SCons.Builder.Builder(**bkw)
    return bld(env, target, source)


def ffmpeg_install(env, target, source):
    return env.InstallAs(get_ffmpeg_install_targets(env, target), get_ffmpeg_install_sources(env, source))
