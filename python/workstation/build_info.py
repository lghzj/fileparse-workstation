import platform
import sys

try:
    from workstation._build_info import BUILD_COMMIT, BUILD_TARGET, BUILD_TIME
except ImportError:
    BUILD_COMMIT = "development"
    BUILD_TARGET = "source"
    BUILD_TIME = "unknown"


def runtime_summary(command: str) -> str:
    return (
        f"target={BUILD_TARGET} commit={BUILD_COMMIT} build_time={BUILD_TIME} "
        f"command={command} python={platform.python_version()} platform={sys.platform} frozen={bool(getattr(sys, 'frozen', False))}"
    )
