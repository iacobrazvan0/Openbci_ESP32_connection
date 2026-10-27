Import("env")

import os

toolchain_dir = env.PioPlatform().get_package_dir("toolchain-gccmingw32")
if toolchain_dir:
    bin_dir = os.path.join(toolchain_dir, "bin")
    env.PrependENVPath("PATH", bin_dir)
    env.Replace(CC=os.path.join(bin_dir, "gcc.exe"))
    env.Replace(CXX=os.path.join(bin_dir, "g++.exe"))
    env.Append(LINKFLAGS=["-static-libgcc", "-static-libstdc++"])
