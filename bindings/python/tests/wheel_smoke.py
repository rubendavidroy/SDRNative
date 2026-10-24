# Check an installed wheel outside the source directory.
from pathlib import Path

import sdrnative


package_dir = Path(sdrnative.__file__).resolve().parent
library = Path(sdrnative.native_library_path()).resolve()
assert library.is_file(), library
assert library.parent == package_dir, (library, package_dir)
assert sdrnative.version() == "0.1.0"
assert sdrnative.abi_version() == 1
assert isinstance(sdrnative.list_devices(), list)
print("SDRNative wheel OK:", library)
