#!/usr/bin/env python3
# Copyright 2026, Tom F.
# SPDX-License-Identifier: BSL-1.0

"""
Monado Phone Driver installer

This script is used to help you with the installation and setup of the Monado Phone Driver.

Usage:
    python installer.py
"""

import os
import sys
import subprocess
from urllib.request import urlretrieve

# Define ANSI escape codes for pretty printing

RESET = "\033[0m"
BOLD = "\033[1m"
RED = "\033[31;91m"
GREEN = "\033[32;92m"
YELLOW = "\033[33;93m"
BLUE = "\033[34;94m"
MAGENTA = "\033[35;95m"
CYAN = "\033[36;96m"

# Pretty print helper functions

def ask(text: str = "Do you want to continue?", abort: bool = True):
    res = input(f"{CYAN}[?]{RESET} {text} (y/n) ")
    if res.lower() == "y":
        return True
    if abort:
        err("Aborted")
    return False

def err(text: str):
    print(f"{RED}[!]{RESET} {text}")
    sys.exit(1)

def warn(text: str):
    print(f"{YELLOW}[!]{RESET} {text}")

def info(text: str):
    print(f"{BLUE}[i]{RESET} {text}")

if __name__ == "__main__":
    try:
        print(GREEN + BOLD + "Monado Phone Driver installer" + RESET)
        print()
        info("This script will help you with the installation and setup of the Monado Phone Driver.")
        info("All files will be downloaded in the current directory, so make sure this script is run in the right directory.")
        print()
        ask()

        info("Downloading files...")
        info("Downloading MonadoPhone.apk...")
        urlretrieve("https://github.com/ttomf/monado-phone/releases/latest/download/MonadoPhone.apk", "MonadoPhone.apk")
        info("Downloading libopenxr_monado.so...")
        urlretrieve("https://github.com/ttomf/monado-phone/releases/latest/download/libopenxr_monado.so", "libopenxr_monado.so")
        info("Downloading monado-service...")
        urlretrieve("https://github.com/ttomf/monado-phone/releases/latest/download/monado-service", "monado-service")
        os.chmod("monado-service", 0o755)
        print()

        info("Creating manifest openxr_monado-dev.json...")
        under_home = ask("Do you want to install the runtime also under $HOME/.config/openxr/1/active_runtime.json?", False)
        if under_home or ask("Do you want to include full path to the runtime in the installed manifest?", False):
            warn("Do not move libopenxr_monado.so after installation without changing the path in the manifest!")
            manifest = """\
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "Monado",
        "library_path": \"""" + os.path.abspath("libopenxr_monado.so") + """\"
    }
}
"""
        else:
            warn("Do not move libopenxr_monado.so after installation without the openxr_monado-dev.json manifest (they must be in the same directory)!")
            manifest = """\
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "Monado",
        "library_path": "libopenxr_monado.so"
    }
}
"""
        if under_home:
            os.makedirs(os.path.join(os.path.expanduser("~"), ".config", "openxr", "1"), exist_ok=True)
            with open(os.path.join(os.path.expanduser("~"), ".config", "openxr", "1", "active_runtime.json"), "w") as f:
                f.write(manifest)
            info("Created " + os.path.join(os.path.expanduser("~"), ".config", "openxr", "1", "active_runtime.json"))
        with open("openxr_monado-dev.json", "w") as f:
            f.write(manifest)
        info("Created openxr_monado-dev.json")
        print()

        if ask("Do you want to install the apk using adb?", False):
            info("Installing MonadoPhone.apk...")
            result = subprocess.run(
                ["adb", "install", "MonadoPhone.apk"],
                capture_output=True,
                text=True
            )
            if result.returncode != 0:
                warn("Failed to install MonadoPhone.apk: " + result.stderr)
            else:
                info("MonadoPhone.apk installed")

        info("Installation complete!")
        print()
        info("To use the driver, open installed apk on phone and run the following command:")
        info("./monado-service")
        if under_home:
            info("Then you can open any OpenXR application.")
        else:
            info("Then you can open any OpenXR application with environment variable set:")
            info("export XR_RUNTIME_JSON=" + os.path.abspath("openxr_monado-dev.json"))
            info("Tip: if you don't want to set the environment variable every time, you can add it to your ~/.bashrc or ~/.zshrc")
            info("     or create a symlink to openxr_monado-dev.json in ~/.config/openxr/1/active_runtime.json")
    except KeyboardInterrupt:
        print()
        err("Aborted")
    except Exception as e:
        print()
        err(f"An error occurred: {e}")
